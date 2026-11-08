// Windows adapter: CD-DA reading through the CD-ROM class driver (cdrom.sys) I/O controls.
// IOCTL_CDROM_RAW_READ returns audio sectors, optionally with C2 error pointers or raw
// sub-channel data; IOCTL_CDROM_READ_TOC_EX returns the full TOC and CD-Text in MMC format;
// IOCTL_CDROM_READ_Q_CHANNEL returns ISRC and MCN. No administrator rights are needed.
// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>

#include "cd.h"
#include "cd_mmc.h"

namespace fiso {

namespace {

// From ntddcdrm.h, defined here so that the code builds with any Windows SDK or MinGW headers.
constexpr DWORD kFileDeviceCdRom = 0x00000002;
constexpr DWORD ctlCode(DWORD function, DWORD method) {
    return (kFileDeviceCdRom << 16) | (FILE_READ_ACCESS << 14) | (function << 2) | method;
}
constexpr DWORD kIoctlCdromReadQChannel = ctlCode(0x000B, METHOD_BUFFERED);
constexpr DWORD kIoctlCdromRawRead = ctlCode(0x000F, METHOD_OUT_DIRECT);
constexpr DWORD kIoctlCdromReadTocEx = ctlCode(0x0015, METHOD_BUFFERED);

enum TrackMode : ULONG { kCdda = 2, kRawWithC2 = 4, kRawWithSubCode = 5 };
struct RawReadInfo {
    LARGE_INTEGER diskOffset;  // in units of 2048-byte sectors, expressed in bytes
    ULONG sectorCount;
    ULONG trackMode;
};
struct ReadTocEx {
    UCHAR formatAndMsf;  // bits 0-3: format, bit 7: MSF
    UCHAR sessionTrack;
    UCHAR reserved2, reserved3;
};
struct SubQDataFormat {
    UCHAR format;  // 2 = media catalog number, 3 = track ISRC
    UCHAR track;
};

constexpr int kMaxSectorsPerRead = 24;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// "D:" for drive letter D.
std::string driveId(char letter) {
    return std::string(1, letter) + ":";
}

std::vector<char> opticalDriveLetters() {
    std::vector<char> v;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_CDROM) v.push_back((char)('A' + i));
    }
    return v;
}

HANDLE openDrive(char letter, bool mustSucceed) {
    wchar_t path[] = {L'\\', L'\\', L'.', L'\\', (wchar_t)letter, L':', 0};
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE && mustSucceed) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) fail(FISO_E_IO, "[permission] Windows denied access to drive " + driveId(letter));
        if (e == ERROR_SHARING_VIOLATION) fail(FISO_E_IO, "[busy] drive " + driveId(letter) + " is used by another application");
        fail(FISO_E_IO, "[no_device] cannot open drive " + driveId(letter) + " (error " + std::to_string(e) + ")");
    }
    return h;
}

bool ioctl(HANDLE h, DWORD code, const void *in, DWORD inLen, void *out, DWORD outLen, DWORD *returned = nullptr) {
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void *>(in), inLen, out, outLen, &got, nullptr);
    if (returned) *returned = got;
    return ok != FALSE;
}

bool mediaPresent(HANDLE h) {
    return ioctl(h, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0);
}

struct DriveIdentity {
    std::string vendor, product, firmware;
};
DriveIdentity identity(HANDLE h) {
    DriveIdentity d;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(1024);
    if (!ioctl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), (DWORD)buf.size())) return d;
    auto *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf.data());
    auto field = [&](DWORD off) { return off && off < buf.size() ? trim((const char *)buf.data() + off) : std::string(); };
    d.vendor = field(desc->VendorIdOffset);
    d.product = field(desc->ProductIdOffset);
    d.firmware = field(desc->ProductRevisionOffset);
    return d;
}

std::string displayName(const DriveIdentity &d, char letter) {
    std::string n = trim(d.vendor + " " + d.product);
    return (n.empty() ? "CD drive" : n) + " (" + driveId(letter) + ")";
}

// Page-aligned buffer, as required by direct I/O on some drives.
class AlignedBuffer {
  public:
    ~AlignedBuffer() {
        if (p_) VirtualFree(p_, 0, MEM_RELEASE);
    }
    uint8_t *get(size_t n) {
        if (n > cap_) {
            if (p_) VirtualFree(p_, 0, MEM_RELEASE);
            p_ = (uint8_t *)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!p_) fail(FISO_E_IO, "out of memory");
            cap_ = n;
        }
        return p_;
    }

  private:
    uint8_t *p_ = nullptr;
    size_t cap_ = 0;
};

class WindowsReader : public cd::Reader {
  public:
    WindowsReader(HANDLE h, std::string name) : h_(h), name_(std::move(name)) {}
    ~WindowsReader() override { CloseHandle(h_); }

    void checkAudioDisc() {
        auto raw = readToc(0x02, true, 1);
        if (raw.empty()) fail(FISO_E_UNSUPPORTED, "[no_media] no readable disc in the drive");
        cd::DiscToc t = cd::parseFullToc(raw.data(), raw.size());
        if (std::none_of(t.tracks.begin(), t.tracks.end(), [](const cd::TocTrack &x) { return x.audio(); }))
            fail(FISO_E_UNSUPPORTED, "[no_audio] the disc has no audio track");
    }

    std::string driveName() override { return name_; }

    cd::DiscToc toc() override {
        auto raw = readToc(0x02, true, 1);
        if (raw.empty()) fail(FISO_E_IO, "cannot read the TOC");
        return cd::parseFullToc(raw.data(), raw.size());
    }

    bool hasC2() override {
        if (c2_ < 0) {
            int32_t lba = 0;
            for (auto &x : toc().tracks)
                if (x.audio()) {
                    lba = x.lba;
                    break;
                }
            c2_ = rawRead(lba, 1, kRawWithC2, cd::kSectorBytes + cd::kC2Bytes) ? 1 : 0;
        }
        return c2_ == 1;
    }

    bool read(int32_t lba, int n, uint8_t *audio, uint8_t *c2) override {
        if (lba < 0 || n <= 0) return false;
        bool withC2 = c2 && hasC2();
        size_t bs = cd::kSectorBytes + (withC2 ? cd::kC2Bytes : 0);
        for (int done = 0; done < n;) {
            int k = std::min(kMaxSectorsPerRead, n - done);
            const uint8_t *p = rawRead(lba + done, k, withC2 ? kRawWithC2 : kCdda, bs);
            if (!p) return false;
            for (int i = 0; i < k; i++) {
                std::memcpy(audio + (size_t)(done + i) * cd::kSectorBytes, p + (size_t)i * bs, cd::kSectorBytes);
                if (withC2)
                    std::memcpy(c2 + (size_t)(done + i) * cd::kC2Bytes, p + (size_t)i * bs + cd::kSectorBytes, cd::kC2Bytes);
            }
            done += k;
        }
        return true;
    }

    bool readQ(int32_t lba, uint8_t *q16) override {
        if (lba < 0) return false;
        const uint8_t *p = rawRead(lba, 1, kRawWithSubCode, cd::kSectorBytes + 96);
        if (!p) return false;
        cd::qFromRawPW(p + cd::kSectorBytes, q16);
        return true;
    }

    std::string isrc(int track) override {
        SubQDataFormat f{0x03, (UCHAR)track};
        uint8_t r[48] = {};
        DWORD got = 0;
        if (!ioctl(h_, kIoctlCdromReadQChannel, &f, sizeof f, r, sizeof r, &got) || got < 21 || !(r[8] & 0x80)) return "";
        return std::string((const char *)r + 9, strnlen((const char *)r + 9, 12));
    }

    std::string mcn() override {
        SubQDataFormat f{0x02, 0};
        uint8_t r[48] = {};
        DWORD got = 0;
        if (!ioctl(h_, kIoctlCdromReadQChannel, &f, sizeof f, r, sizeof r, &got) || got < 22 || !(r[8] & 0x80)) return "";
        return std::string((const char *)r + 9, strnlen((const char *)r + 9, 13));
    }

    std::vector<uint8_t> cdText() override {
        auto raw = readToc(0x05, false, 0);
        if (raw.size() < 4 + 18) return {};
        size_t n = 4 + (raw.size() - 4) / 18 * 18;
        return std::vector<uint8_t>(raw.begin() + 4, raw.begin() + (long)n);
    }

  private:
    // MMC READ TOC response (4-byte header + descriptors), as returned by the drive.
    std::vector<uint8_t> readToc(UCHAR format, bool msf, UCHAR session) {
        ReadTocEx in{(UCHAR)(format | (msf ? 0x80 : 0)), session, 0, 0};
        std::vector<uint8_t> out(65536);
        DWORD got = 0;
        if (!ioctl(h_, kIoctlCdromReadTocEx, &in, sizeof in, out.data(), (DWORD)out.size(), &got) || got < 4) return {};
        size_t len = std::min<size_t>(got, (size_t)((out[0] << 8) | out[1]) + 2);
        out.resize(len);
        return out;
    }

    const uint8_t *rawRead(int32_t lba, int n, TrackMode mode, size_t bytesPerSector) {
        RawReadInfo in{};
        in.diskOffset.QuadPart = (LONGLONG)lba * 2048;
        in.sectorCount = (ULONG)n;
        in.trackMode = mode;
        size_t bytes = (size_t)n * bytesPerSector;
        uint8_t *p = buf_.get(bytes);
        DWORD got = 0;
        if (!ioctl(h_, kIoctlCdromRawRead, &in, sizeof in, p, (DWORD)bytes, &got) || got != bytes) return nullptr;
        return p;
    }

    HANDLE h_;
    std::string name_;
    int c2_ = -1;
    AlignedBuffer buf_;
};

char pickDrive(const std::string &id) {
    auto letters = opticalDriveLetters();
    if (!id.empty()) {
        char want = (char)toupper((unsigned char)id[0]);
        if (std::find(letters.begin(), letters.end(), want) != letters.end()) return want;
        fail(FISO_E_ARG, "[no_device] drive '" + id + "' not found");
    }
    if (letters.empty()) fail(FISO_E_UNSUPPORTED, "[no_device] no CD drive was found");
    for (char l : letters) {  // the first drive that holds a disc
        HANDLE h = openDrive(l, false);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool present = mediaPresent(h);
        CloseHandle(h);
        if (present) return l;
    }
    return letters[0];
}

}  // namespace

json platformDrives() {
    json out = json::array();
    for (char l : opticalDriveLetters()) {
        json drive = {{"id", driveId(l)},
                      {"name", "CD drive (" + driveId(l) + ")"},
                      {"can_read", true},
                      {"media", {{"state", "none"}, {"class_cd", true}, {"readable", false}}}};
        HANDLE h = openDrive(l, false);
        if (h != INVALID_HANDLE_VALUE) {
            DriveIdentity d = identity(h);
            drive["vendor"] = d.vendor;
            drive["product"] = d.product;
            drive["firmware"] = d.firmware;
            drive["name"] = displayName(d, l);
            if (mediaPresent(h)) {
                drive["media"]["state"] = "present";
                drive["media"]["readable"] = true;
            }
            CloseHandle(h);
        }
        out.push_back(drive);
    }
    return {{"platform", "windows"}, {"drives", out}};
}

std::unique_ptr<cd::Reader> platformOpenReader(const std::string &device) {
    char l = pickDrive(device);
    HANDLE h = openDrive(l, true);
    if (!mediaPresent(h)) {
        CloseHandle(h);
        fail(FISO_E_UNSUPPORTED, "[no_media] no disc in the drive");
    }
    auto reader = std::make_unique<WindowsReader>(h, displayName(identity(h), l));
    reader->checkAudioDisc();
    return reader;
}

void platformEject(const std::string &device) {
    char l = pickDrive(device);
    HANDLE h = openDrive(l, true);
    PREVENT_MEDIA_REMOVAL allow{FALSE};
    ioctl(h, IOCTL_STORAGE_MEDIA_REMOVAL, &allow, sizeof allow, nullptr, 0);
    bool ok = ioctl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) {
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_BUSY) fail(FISO_E_IO, "[busy] the disc is used by another application");
        fail(FISO_E_IO, "eject failed (error " + std::to_string(e) + ")");
    }
}

}  // namespace fiso
#endif  // _WIN32
