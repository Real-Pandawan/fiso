// CD reading with standard MMC commands. SPDX-License-Identifier: Apache-2.0
#include "cd_mmc.h"

#include <algorithm>
#include <cstring>

namespace fiso::cd {

namespace {
enum : uint8_t { kReadSubChannel = 0x42, kReadToc = 0x43, kReadCd = 0xBE };
enum : uint8_t { kTocFormatFull = 0x02, kTocFormatCdText = 0x05 };
enum : uint8_t { kSubQ = 0x40, kSubFormatMcn = 0x02, kSubFormatIsrc = 0x03 };
// READ CD byte 1: expected sector type CD-DA; byte 9: user data, C2 error pointers (294 bytes).
enum : uint8_t { kSectorTypeCdda = 0x04, kUserData = 0x10, kC2Pointers = 0x02, kRawPW = 0x01 };
constexpr int kMaxSectorsPerCommand = 24;  // stays under the common 64 KiB transfer limit with C2

void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
}  // namespace

void qFromRawPW(const uint8_t *pw, uint8_t *q) {
    std::memset(q, 0, 16);
    for (int i = 0; i < 96; i++)
        if (pw[i] & 0x40) q[i / 8] |= (uint8_t)(0x80 >> (i % 8));
}

std::vector<uint8_t> MmcReader::readTocFormat(uint8_t format, bool msf, uint8_t session) {
    uint8_t cdb[10] = {kReadToc, (uint8_t)(msf ? 0x02 : 0x00), format, 0, 0, 0, session, 0, 4, 0};
    uint8_t head[4] = {};
    if (t_.command(cdb, sizeof cdb, head, sizeof head) < 4) return {};
    size_t len = (size_t)((head[0] << 8) | head[1]) + 2;
    if (len < 4) return {};
    std::vector<uint8_t> out(len);
    cdb[7] = (uint8_t)(len >> 8);
    cdb[8] = (uint8_t)len;
    long got = t_.command(cdb, sizeof cdb, out.data(), out.size());
    if (got < 4) return {};
    out.resize((size_t)got);
    return out;
}

void MmcReader::checkAudioDisc() {
    auto raw = readTocFormat(kTocFormatFull, true, 1);
    if (raw.empty()) fail(FISO_E_UNSUPPORTED, "[no_media] no readable disc in the drive");
    DiscToc t = parseFullToc(raw.data(), raw.size());
    if (std::none_of(t.tracks.begin(), t.tracks.end(), [](const TocTrack &x) { return x.audio(); }))
        fail(FISO_E_UNSUPPORTED, "[no_audio] the disc has no audio track");
}

DiscToc MmcReader::toc() {
    auto raw = readTocFormat(kTocFormatFull, true, 1);
    if (raw.empty()) fail(FISO_E_IO, "cannot read the TOC");
    return parseFullToc(raw.data(), raw.size());
}

bool MmcReader::readCd(int32_t lba, int n, uint8_t flags, uint8_t subchannel, uint8_t *out, size_t bytes) {
    uint8_t cdb[12] = {kReadCd, kSectorTypeCdda, 0, 0, 0, 0, 0, 0, 0, flags, subchannel, 0};
    be32(cdb + 2, (uint32_t)lba);
    cdb[6] = (uint8_t)(n >> 16);
    cdb[7] = (uint8_t)(n >> 8);
    cdb[8] = (uint8_t)n;
    return t_.command(cdb, sizeof cdb, out, bytes) == (long)bytes;
}

bool MmcReader::hasC2() {
    if (c2_ < 0) {
        int32_t lba = 0;
        for (auto &x : toc().tracks)
            if (x.audio()) {
                lba = x.lba;
                break;
            }
        std::vector<uint8_t> probe(kSectorBytes + kC2Bytes);
        c2_ = readCd(lba, 1, kUserData | kC2Pointers, 0, probe.data(), probe.size()) ? 1 : 0;
    }
    return c2_ == 1;
}

bool MmcReader::read(int32_t lba, int n, uint8_t *audio, uint8_t *c2) {
    if (lba < 0 || n <= 0) return false;
    bool withC2 = c2 && hasC2();
    size_t bs = kSectorBytes + (withC2 ? kC2Bytes : 0);
    for (int done = 0; done < n;) {
        int k = std::min(kMaxSectorsPerCommand, n - done);
        buf_.resize((size_t)k * bs);
        if (!readCd(lba + done, k, kUserData | (withC2 ? kC2Pointers : 0), 0, buf_.data(), buf_.size())) return false;
        for (int i = 0; i < k; i++) {
            std::memcpy(audio + (size_t)(done + i) * kSectorBytes, buf_.data() + (size_t)i * bs, kSectorBytes);
            if (withC2) std::memcpy(c2 + (size_t)(done + i) * kC2Bytes, buf_.data() + (size_t)i * bs + kSectorBytes, kC2Bytes);
        }
        done += k;
    }
    return true;
}

bool MmcReader::readQ(int32_t lba, uint8_t *q16) {
    if (lba < 0) return false;
    uint8_t sector[kSectorBytes + 96];
    if (!readCd(lba, 1, kUserData, kRawPW, sector, sizeof sector)) return false;
    qFromRawPW(sector + kSectorBytes, q16);
    return true;
}

std::string MmcReader::isrc(int track) {
    uint8_t cdb[10] = {kReadSubChannel, 0, kSubQ, kSubFormatIsrc, 0, 0, (uint8_t)track, 0, 24, 0};
    uint8_t r[24] = {};
    if (t_.command(cdb, sizeof cdb, r, sizeof r) < 21 || !(r[8] & 0x80)) return "";
    return std::string((const char *)r + 9, strnlen((const char *)r + 9, 12));
}

std::string MmcReader::mcn() {
    uint8_t cdb[10] = {kReadSubChannel, 0, kSubQ, kSubFormatMcn, 0, 0, 0, 0, 24, 0};
    uint8_t r[24] = {};
    if (t_.command(cdb, sizeof cdb, r, sizeof r) < 22 || !(r[8] & 0x80)) return "";
    return std::string((const char *)r + 9, strnlen((const char *)r + 9, 13));
}

std::vector<uint8_t> MmcReader::cdText() {
    auto raw = readTocFormat(kTocFormatCdText, false, 0);
    if (raw.size() < 4 + 18) return {};
    size_t n = 4 + (raw.size() - 4) / 18 * 18;
    return std::vector<uint8_t>(raw.begin() + 4, raw.begin() + (long)n);
}

}  // namespace fiso::cd
