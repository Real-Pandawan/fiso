// CD-DA reading and AccurateRip — portable core.
// Platform adapters (cd_macos.cpp) implement cd::Reader, drive listing and eject.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "internal.h"

namespace fiso::cd {

constexpr uint32_t kSectorBytes = 2352;
constexpr uint32_t kC2Bytes = 294;

struct TocTrack {
    int number = 0;
    int session = 1;
    uint8_t control = 0, adr = 1;
    int32_t lba = 0;
    bool audio() const { return !(control & 4); }
};
struct DiscToc {
    std::vector<TocTrack> tracks;
    std::vector<std::pair<int, int32_t>> leadouts;  // (session, lba)
    int32_t leadout(int session) const;
};
// Parses the "full TOC" (READ TOC format 0010b) as returned by the drive:
// 4-byte header then 11-byte descriptors with binary MSF values.
DiscToc parseFullToc(const uint8_t *p, size_t n);

struct QInfo {
    bool ok = false;
    uint8_t control = 0, adr = 0, track = 0, index = 0;
};
QInfo parseQ(const uint8_t *q16);  // validates the sub-channel Q CRC

// CD-Text block 0 (titles and performers), packs of 18 bytes, header optional.
json parseCdText(const uint8_t *p, size_t n);

// Hardware access used by the ripper. All addresses are LBAs.
class Reader {
  public:
    virtual ~Reader() = default;
    virtual std::string driveName() = 0;
    virtual DiscToc toc() = 0;
    virtual bool hasC2() = 0;
    // Reads n CD-DA sectors (little-endian PCM). c2 receives n*294 bytes of error flags when
    // hasC2(); may be null. Returns false when the drive reports a read error.
    virtual bool read(int32_t lba, int n, uint8_t *audio, uint8_t *c2) = 0;
    virtual bool readQ(int32_t lba, uint8_t *q16) = 0;
    virtual std::string isrc(int track) = 0;
    virtual std::string mcn() = 0;
    virtual std::vector<uint8_t> cdText() = 0;
};

json discInfo(Reader &r);
// options: output, mode ("secure"|"fast"), read_offset, retries, meta, pictures,
// compression, overwrite, detect_pregaps, read_htoa
json rip(Reader &r, const json &options, const Progress &prog);

// ---- AccurateRip
struct ArTrack {
    int number;
    uint32_t v1, v2;
};
struct ArResult {
    uint32_t id1 = 0, id2 = 0, cddb = 0;
    int tracks = 0;
    std::string url;
    std::vector<ArTrack> crcs;
};
ArResult accurateRip(Image &img, const Progress &prog);  // requires a CD-DA image
json accurateRipCompare(const ArResult &r, const std::vector<uint8_t> &db);

}  // namespace fiso::cd

namespace fiso {
// Platform layer (cd_macos.cpp or cd_none.cpp)
json platformDrives();
std::unique_ptr<cd::Reader> platformOpenReader(const std::string &device);
void platformEject(const std::string &device);
}  // namespace fiso
