// macOS adapter: CD-DA reading through the IOCDMedia BSD client ioctls; drive listing
// and eject through the DiscRecording framework. Frameworks are loaded with dlopen so that the
// tool builds without the Apple SDK and has no link-time framework dependency.
// Structures and constants follow IOKit/storage/IOCDMediaBSDClient.h, IOCDTypes.h and
// DiscRecording/DRCore*.h (macOS 11.3 SDK).
// SPDX-License-Identifier: Apache-2.0
#ifdef __APPLE__
#include "cd.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

namespace fiso {

// ------------------------------------------------------------------ IOCDMediaBSDClient.h (LP64)
namespace iocd {
typedef struct {
    uint64_t offset;
    uint8_t sectorArea;
    uint8_t sectorType;
    uint8_t reserved0080[10];
    uint32_t bufferLength;
    void *buffer;
} dk_cd_read_t;
typedef struct {
    char isrc[13];
    uint8_t track;
    uint8_t reserved0112[2];
} dk_cd_read_isrc_t;
typedef struct {
    char mcn[14];
    uint8_t reserved0112[2];
} dk_cd_read_mcn_t;
typedef struct {
    uint8_t format;
    uint8_t formatAsTime;
    uint8_t reserved0016[5];
    union {
        uint8_t session;
        uint8_t track;
    } address;
    uint8_t reserved0064[6];
    uint16_t bufferLength;
    void *buffer;
} dk_cd_read_toc_t;
#define FISO_DKIOCCDREAD _IOWR('d', 96, fiso::iocd::dk_cd_read_t)
#define FISO_DKIOCCDREADISRC _IOWR('d', 97, fiso::iocd::dk_cd_read_isrc_t)
#define FISO_DKIOCCDREADMCN _IOWR('d', 98, fiso::iocd::dk_cd_read_mcn_t)
#define FISO_DKIOCCDREADTOC _IOWR('d', 100, fiso::iocd::dk_cd_read_toc_t)
enum { AreaUser = 0x10, AreaErrorFlags = 0x02, AreaSubChannelQ = 0x04, TypeCDDA = 0x01 };
enum { TocFormatTOC = 0x02, TocFormatText = 0x05 };
}  // namespace iocd

// ------------------------------------------------------------------ CoreFoundation / DiscRecording (loaded with dlopen)
typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFArrayRef;
typedef const void *CFDictionaryRef;
typedef const void *CFBooleanRef;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef uint32_t CFStringEncoding;
typedef int32_t OSStatus;
typedef void *DRDeviceRef;
constexpr CFStringEncoding kUTF8 = 0x08000100;

struct Api {
    bool ok = false;
    std::string err;
    void *cf = nullptr, *dr = nullptr;
    // CoreFoundation
    void (*CFRelease)(CFTypeRef);
    CFTypeRef (*CFRetain)(CFTypeRef);
    CFTypeID (*CFGetTypeID)(CFTypeRef);
    CFTypeID (*CFStringGetTypeID)();
    CFTypeID (*CFBooleanGetTypeID)();
    CFTypeID (*CFDictionaryGetTypeID)();
    CFIndex (*CFArrayGetCount)(CFArrayRef);
    const void *(*CFArrayGetValueAtIndex)(CFArrayRef, CFIndex);
    const void *(*CFDictionaryGetValue)(CFDictionaryRef, const void *);
    unsigned char (*CFStringGetCString)(CFStringRef, char *, CFIndex, CFStringEncoding);
    unsigned char (*CFBooleanGetValue)(CFBooleanRef);
    unsigned char (*CFEqual)(CFTypeRef, CFTypeRef);
    // DiscRecording
    CFArrayRef (*DRCopyDeviceArray)();
    CFDictionaryRef (*DRDeviceCopyInfo)(DRDeviceRef);
    CFDictionaryRef (*DRDeviceCopyStatus)(DRDeviceRef);
    OSStatus (*DRDeviceEjectMedia)(DRDeviceRef);
    std::map<std::string, CFStringRef> k;  // DiscRecording string constants

    CFStringRef K(const char *name) {
        auto it = k.find(name);
        if (it == k.end()) fail(FISO_E_INTERNAL, std::string("DiscRecording constant missing: ") + name);
        return it->second;
    }
};

static Api &api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        a.cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
        a.dr = dlopen("/System/Library/Frameworks/DiscRecording.framework/DiscRecording", RTLD_NOW);
        if (!a.cf || !a.dr) {
            a.err = "cannot load CoreFoundation/DiscRecording frameworks";
            return;
        }
        bool missing = false;
        auto sym = [&](void *h, const char *n) {
            void *p = dlsym(h, n);
            if (!p) {
                missing = true;
                a.err = std::string("missing symbol ") + n;
            }
            return p;
        };
#define CF(n) a.n = (decltype(a.n))sym(a.cf, #n)
#define DR(n) a.n = (decltype(a.n))sym(a.dr, #n)
        CF(CFRelease);
        CF(CFRetain);
        CF(CFGetTypeID);
        CF(CFStringGetTypeID);
        CF(CFBooleanGetTypeID);
        CF(CFDictionaryGetTypeID);
        CF(CFArrayGetCount);
        CF(CFArrayGetValueAtIndex);
        CF(CFDictionaryGetValue);
        CF(CFStringGetCString);
        CF(CFBooleanGetValue);
        CF(CFEqual);
        DR(DRCopyDeviceArray);
        DR(DRDeviceCopyInfo);
        DR(DRDeviceCopyStatus);
        DR(DRDeviceEjectMedia);
#undef CF
#undef DR
        static const char *names[] = {
            "kDRDeviceVendorNameKey",  "kDRDeviceProductNameKey",         "kDRDeviceFirmwareRevisionKey",
            "kDRDeviceIsBusyKey",      "kDRDeviceIsTrayOpenKey",          "kDRDeviceMediaStateKey",
            "kDRDeviceMediaInfoKey",   "kDRDeviceMediaStateMediaPresent", "kDRDeviceMediaStateInTransition",
            "kDRDeviceMediaStateNone", "kDRDeviceMediaBSDNameKey",        "kDRDeviceMediaIsBlankKey",
            "kDRDeviceMediaClassKey",  "kDRDeviceMediaTypeKey",           "kDRDeviceMediaClassCD",
            "kDRDeviceMediaTypeCDROM", "kDRDeviceMediaTypeCDR",           "kDRDeviceMediaTypeCDRW",
        };
        for (auto n : names) {
            auto p = (const void *const *)dlsym(a.dr, n);
            if (!p) {
                missing = true;
                a.err = std::string("missing DiscRecording symbol ") + n;
                continue;
            }
            a.k[n] = *p;
        }
        a.ok = !missing;
    });
    if (!a.ok) fail(FISO_E_UNSUPPORTED, "[no_discrecording] " + a.err);
    return a;
}

// ------------------------------------------------------------------ CF helpers
struct CFHold {
    CFTypeRef p;
    explicit CFHold(CFTypeRef x = nullptr) : p(x) {}
    ~CFHold() {
        if (p) api().CFRelease(p);
    }
    CFHold(const CFHold &) = delete;
    CFHold &operator=(const CFHold &) = delete;
    operator CFTypeRef() const { return p; }
    void reset(CFTypeRef x) {
        if (p) api().CFRelease(p);
        p = x;
    }
};
static std::string str(CFTypeRef s) {
    auto &A = api();
    if (!s || A.CFGetTypeID(s) != A.CFStringGetTypeID()) return "";
    char buf[1024];
    return A.CFStringGetCString(s, buf, sizeof buf, kUTF8) ? std::string(buf) : std::string();
}
static bool boolean(CFTypeRef b) {
    auto &A = api();
    return b && A.CFGetTypeID(b) == A.CFBooleanGetTypeID() && A.CFBooleanGetValue(b);
}
static CFTypeRef get(CFDictionaryRef d, const char *key) {
    auto &A = api();
    if (!d || A.CFGetTypeID(d) != A.CFDictionaryGetTypeID()) return nullptr;
    return A.CFDictionaryGetValue(d, A.K(key));
}
static bool same(CFTypeRef a, const char *key) {
    return a && api().CFEqual(a, api().K(key));
}

// ------------------------------------------------------------------ devices
struct DeviceEntry {
    std::string id;
    DRDeviceRef dr = nullptr;
    std::string bsd, name;
};

static std::vector<DeviceEntry> listDevices(json *out) {
    auto &A = api();
    std::vector<DeviceEntry> v;
    CFHold arr(A.DRCopyDeviceArray());
    CFIndex n = arr ? A.CFArrayGetCount(arr) : 0;
    for (CFIndex i = 0; i < n; i++) {
        DRDeviceRef dev = (DRDeviceRef)A.CFArrayGetValueAtIndex(arr, i);
        CFHold info(A.DRDeviceCopyInfo(dev)), status(A.DRDeviceCopyStatus(dev));
        DeviceEntry e;
        e.id = "dr:" + std::to_string(i);
        e.dr = (DRDeviceRef)A.CFRetain(dev);
        std::string vendor = str(get(info, "kDRDeviceVendorNameKey")), product = str(get(info, "kDRDeviceProductNameKey"));
        e.name = vendor + " " + product;
        CFTypeRef media = get(status, "kDRDeviceMediaInfoKey");
        e.bsd = str(get(media, "kDRDeviceMediaBSDNameKey"));
        if (out) {
            CFTypeRef state = get(status, "kDRDeviceMediaStateKey");
            std::string st = same(state, "kDRDeviceMediaStateMediaPresent")   ? "present"
                             : same(state, "kDRDeviceMediaStateInTransition") ? "transition"
                                                                              : "none";
            CFTypeRef type = get(media, "kDRDeviceMediaTypeKey");
            std::string mt = same(type, "kDRDeviceMediaTypeCDR")     ? "CD-R"
                             : same(type, "kDRDeviceMediaTypeCDRW")  ? "CD-RW"
                             : same(type, "kDRDeviceMediaTypeCDROM") ? "CD-ROM"
                                                                     : str(type);
            out->push_back({{"id", e.id},
                            {"vendor", vendor},
                            {"product", product},
                            {"firmware", str(get(info, "kDRDeviceFirmwareRevisionKey"))},
                            {"name", e.name},
                            {"can_read", true},
                            {"busy", boolean(get(status, "kDRDeviceIsBusyKey"))},
                            {"tray_open", boolean(get(status, "kDRDeviceIsTrayOpenKey"))},
                            {"media",
                             {{"state", st},
                              {"class_cd", same(get(media, "kDRDeviceMediaClassKey"), "kDRDeviceMediaClassCD")},
                              {"type", mt},
                              {"blank", boolean(get(media, "kDRDeviceMediaIsBlankKey"))},
                              {"bsd_name", e.bsd},
                              {"readable", st == "present" && !e.bsd.empty()}}}});
        }
        v.push_back(e);
    }
    return v;
}

static void releaseDevices(std::vector<DeviceEntry> &v) {
    for (auto &e : v)
        if (e.dr) api().CFRelease(e.dr);
}

static int openRaw(const std::string &bsd) {
    std::string path = "/dev/r" + bsd;
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fail(FISO_E_IO, "[permission] macOS refused access to " + path + " (" + strerror(errno) + ")");
        if (errno == EBUSY) fail(FISO_E_IO, "[busy] the disc is in use by another application");
        fail(FISO_E_IO, "cannot open " + path + ": " + strerror(errno));
    }
    return fd;
}

static bool isAudioCd(int fd) {
    std::vector<uint8_t> buf(2048);
    iocd::dk_cd_read_toc_t t{};
    t.format = iocd::TocFormatTOC;
    t.formatAsTime = 1;
    t.bufferLength = (uint16_t)buf.size();
    t.buffer = buf.data();
    return ioctl(fd, FISO_DKIOCCDREADTOC, &t) == 0;
}

// ------------------------------------------------------------------ reader
class MacReader : public cd::Reader {
  public:
    MacReader(const std::string &bsd, const std::string &name) : bsd_(bsd), name_(name) {
        fd_ = openRaw(bsd);
        if (!isAudioCd(fd_)) {
            close(fd_);
            fd_ = -1;
            fail(FISO_E_UNSUPPORTED, "[not_cd] the disc in the drive is not a CD");
        }
    }
    ~MacReader() override {
        if (fd_ >= 0) close(fd_);
    }
    std::string driveName() override { return name_; }
    cd::DiscToc toc() override {
        std::vector<uint8_t> buf(8192);
        iocd::dk_cd_read_toc_t t{};
        t.format = iocd::TocFormatTOC;
        t.formatAsTime = 1;
        t.bufferLength = (uint16_t)buf.size();
        t.buffer = buf.data();
        if (ioctl(fd_, FISO_DKIOCCDREADTOC, &t) != 0) fail(FISO_E_IO, std::string("cannot read the TOC: ") + strerror(errno));
        return cd::parseFullToc(buf.data(), t.bufferLength);
    }
    bool hasC2() override {
        if (c2_ < 0) {
            auto t = toc();
            int32_t lba = 0;
            for (auto &x : t.tracks)
                if (x.audio()) {
                    lba = x.lba;
                    break;
                }
            std::vector<uint8_t> a(cd::kSectorBytes), c(cd::kC2Bytes);
            c2_ = 1;
            c2_ = read(lba, 1, a.data(), c.data()) ? 1 : 0;
        }
        return c2_ == 1;
    }
    bool read(int32_t lba, int n, uint8_t *audio, uint8_t *c2) override {
        if (lba < 0 || n <= 0) return false;
        bool withC2 = c2 && c2_ != 0;
        uint32_t bs = cd::kSectorBytes + (withC2 ? cd::kC2Bytes : 0);
        buf_.resize((size_t)n * bs);
        iocd::dk_cd_read_t r{};
        r.offset = (uint64_t)lba * bs;
        r.sectorArea = iocd::AreaUser | (withC2 ? iocd::AreaErrorFlags : 0);
        r.sectorType = iocd::TypeCDDA;
        r.bufferLength = (uint32_t)buf_.size();
        r.buffer = buf_.data();
        if (ioctl(fd_, FISO_DKIOCCDREAD, &r) != 0 || r.bufferLength != buf_.size()) return false;
        for (int i = 0; i < n; i++) {
            memcpy(audio + (size_t)i * cd::kSectorBytes, buf_.data() + (size_t)i * bs, cd::kSectorBytes);
            if (withC2) memcpy(c2 + (size_t)i * cd::kC2Bytes, buf_.data() + (size_t)i * bs + cd::kSectorBytes, cd::kC2Bytes);
        }
        return true;
    }
    bool readQ(int32_t lba, uint8_t *q16) override {
        if (lba < 0) return false;
        iocd::dk_cd_read_t r{};
        r.offset = (uint64_t)lba * 16;
        r.sectorArea = iocd::AreaSubChannelQ;
        r.sectorType = iocd::TypeCDDA;
        r.bufferLength = 16;
        r.buffer = q16;
        return ioctl(fd_, FISO_DKIOCCDREAD, &r) == 0 && r.bufferLength == 16;
    }
    std::string isrc(int track) override {
        iocd::dk_cd_read_isrc_t r{};
        r.track = (uint8_t)track;
        if (ioctl(fd_, FISO_DKIOCCDREADISRC, &r) != 0) return "";
        return std::string(r.isrc, strnlen(r.isrc, 12));
    }
    std::string mcn() override {
        iocd::dk_cd_read_mcn_t r{};
        if (ioctl(fd_, FISO_DKIOCCDREADMCN, &r) != 0) return "";
        return std::string(r.mcn, strnlen(r.mcn, 13));
    }
    std::vector<uint8_t> cdText() override {
        std::vector<uint8_t> buf(65532);
        iocd::dk_cd_read_toc_t t{};
        t.format = iocd::TocFormatText;
        t.bufferLength = (uint16_t)buf.size();
        t.buffer = buf.data();
        if (ioctl(fd_, FISO_DKIOCCDREADTOC, &t) != 0 || t.bufferLength < 4 + 18) return {};
        size_t n = std::min<size_t>(t.bufferLength, (size_t)((buf[0] << 8) | buf[1]) + 2);
        if (n < 4 + 18) return {};
        n = 4 + (n - 4) / 18 * 18;
        return std::vector<uint8_t>(buf.begin() + 4, buf.begin() + n);
    }

  private:
    std::string bsd_, name_;
    int fd_ = -1;
    int c2_ = -1;
    std::vector<uint8_t> buf_;
};

// CD drives that DiscRecording does not list (read-only drives) are found by probing.
static std::vector<std::string> probeCdDisks(const std::vector<std::string> &known) {
    std::vector<std::string> res;
    DIR *d = opendir("/dev");
    if (!d) return res;
    while (auto *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("disk", 0) != 0 || n.find('s', 4) != std::string::npos) continue;
        if (std::find(known.begin(), known.end(), n) != known.end()) continue;
        int fd = open(("/dev/r" + n).c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (isAudioCd(fd)) res.push_back(n);
        close(fd);
    }
    closedir(d);
    return res;
}

json platformDrives() {
    json out = json::array();
    std::vector<std::string> known;
    try {
        auto v = listDevices(&out);
        for (auto &e : v)
            if (!e.bsd.empty()) known.push_back(e.bsd);
        releaseDevices(v);
    } catch (const Error &e) {
        // DiscRecording unavailable: still list readable discs
    }
    for (auto &b : probeCdDisks(known))
        out.push_back({{"id", "bsd:" + b},
                       {"vendor", ""},
                       {"product", "CD (" + b + ")"},
                       {"name", "CD drive (" + b + ")"},
                       {"can_read", true},
                       {"media", {{"state", "present"}, {"bsd_name", b}, {"class_cd", true}, {"readable", true}}}});
    return {{"platform", "macos"}, {"drives", out}};
}

static DeviceEntry pickDevice(const std::string &id) {
    if (id.rfind("bsd:", 0) == 0) {
        DeviceEntry e;
        e.id = id;
        e.bsd = id.substr(4);
        e.name = "CD drive (" + e.bsd + ")";
        return e;
    }
    json info = json::array();
    auto v = listDevices(&info);
    DeviceEntry chosen;
    for (size_t i = 0; i < v.size(); i++) {
        bool match = id.empty() ? !v[i].bsd.empty() : v[i].id == id;
        if (match && !chosen.dr) {
            chosen = v[i];
            chosen.dr = (DRDeviceRef)api().CFRetain(v[i].dr);
        }
    }
    releaseDevices(v);
    if (!chosen.dr) {
        if (!id.empty()) fail(FISO_E_ARG, "[no_device] drive '" + id + "' not found");
        auto probed = probeCdDisks({});
        if (probed.empty()) fail(FISO_E_UNSUPPORTED, "[no_media] no audio CD found in any drive");
        chosen.id = "bsd:" + probed[0];
        chosen.bsd = probed[0];
        chosen.name = "CD drive (" + probed[0] + ")";
    }
    return chosen;
}

std::unique_ptr<cd::Reader> platformOpenReader(const std::string &device) {
    DeviceEntry e = pickDevice(device);
    std::string bsd = e.bsd, name = e.name;
    if (e.dr) api().CFRelease(e.dr);
    if (bsd.empty()) fail(FISO_E_UNSUPPORTED, "[no_media] no disc in the drive");
    return std::make_unique<MacReader>(bsd, name);
}

void platformEject(const std::string &device) {
    DeviceEntry e = pickDevice(device);
    if (!e.dr) fail(FISO_E_UNSUPPORTED, "this drive cannot be ejected by FISO; use the Finder");
    OSStatus st = api().DRDeviceEjectMedia(e.dr);
    api().CFRelease(e.dr);
    if (st != 0) fail(FISO_E_IO, "eject failed (" + std::to_string(st) + ")");
}

}  // namespace fiso
#endif  // __APPLE__
