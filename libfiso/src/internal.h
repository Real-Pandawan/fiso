// libfiso internal C++ API — SPDX-License-Identifier: Apache-2.0
#pragma once

#include "fiso/fiso.h"
#include "../third_party/nlohmann/json.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fiso {

using json = nlohmann::json;
using Hash = std::array<uint8_t, 32>;

// ---------------------------------------------------------------- errors
struct Error : std::runtime_error {
    int code;
    Error(int c, const std::string &m) : std::runtime_error(m), code(c) {}
};
[[noreturn]] void fail(int code, const std::string &msg);

// ---------------------------------------------------------------- constants
constexpr uint8_t kMagic[8] = {0x89, 'F', 'I', 'S', 'O', 0x0D, 0x0A, 0x0A};
constexpr uint32_t kHeaderSize = 96;
constexpr uint32_t kEntrySize = 64;
constexpr uint32_t kMaxChunks = 65536;
constexpr uint64_t kMaxMeta = 16ull << 20;
constexpr uint64_t kMaxPict = 64ull << 20;
constexpr uint32_t kSectorSamples = 588;
constexpr uint32_t kChunkCritical = 1;

enum LaytFlags : uint16_t { LF_CDDA = 1, LF_HTOA = 2, LF_PHYSICAL = 4, LF_HAS_TOC = 8, LF_SYNTH_SILENCE = 16 };

// ---------------------------------------------------------------- hashing
class Sha256 {
  public:
    Sha256();
    void update(const void *data, size_t len);
    Hash finish();

  private:
    void block(const uint8_t *p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buflen_ = 0;
    uint64_t total_ = 0;
};
std::string sha1_raw(const std::string &data);  // 20 raw bytes
uint32_t crc32(const void *data, size_t len, uint32_t crc = 0);
std::string hex(const uint8_t *p, size_t n);
inline std::string hex(const Hash &h) {
    return hex(h.data(), h.size());
}

// ---------------------------------------------------------------- binary io
inline void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
inline void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff);
}
inline void put64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; i++) v.push_back((x >> (8 * i)) & 0xff);
}
inline void putz(std::vector<uint8_t> &v, size_t n) {
    v.insert(v.end(), n, 0);
}
inline void putstr(std::vector<uint8_t> &v, const std::string &s, size_t field) {
    for (size_t i = 0; i < field; i++) v.push_back(i < s.size() ? (uint8_t)s[i] : 0);
}
inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}
std::string getstr(const uint8_t *p, size_t field);  // stops at NUL

// ---------------------------------------------------------------- files
class File {
  public:
    File() = default;
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&o) noexcept : f_(o.f_), path_(std::move(o.path_)) { o.f_ = nullptr; }
    File &operator=(File &&o) noexcept;
    static File open(const std::string &path, const char *mode);  // throws
    bool isOpen() const { return f_ != nullptr; }
    void seek(uint64_t pos);
    uint64_t tell();
    uint64_t size();
    void read(void *buf, size_t n);         // exact, throws
    size_t readSome(void *buf, size_t n);   // may return less at EOF
    void write(const void *buf, size_t n);  // exact, throws
    void sync();
    void close();
    const std::string &path() const { return path_; }

  private:
    FILE *f_ = nullptr;
    std::string path_;
};
bool pathExists(const std::string &p);
void removeFile(const std::string &p) noexcept;
void renameReplace(const std::string &from, const std::string &to, bool overwrite);  // atomic
std::string tempPathFor(const std::string &target);
std::string joinPath(const std::string &dir, const std::string &name);
std::string dirName(const std::string &p);
std::string baseName(const std::string &p);
std::vector<uint8_t> readWholeFile(const std::string &p, uint64_t max);
void makeDirs(const std::string &p);

// ---------------------------------------------------------------- misc
std::string uuid4();
std::string nowIso8601();
std::string sanitizeFilename(const std::string &s);
std::string msf(uint64_t sectors);  // mm:ss:ff

// ---------------------------------------------------------------- layout model
struct TrackRec {
    uint8_t number = 0, flags = 0;
    uint16_t first_index = 0, index_count = 0;
    uint64_t start = 0, length = 0;
    std::string isrc;
};
struct IndexRec {
    uint8_t track = 0, index = 0;
    uint64_t pos = 0;
};
struct SegmentRec {
    uint32_t chunk_index = 0;
    uint8_t kind = 1, track = 0;
    uint64_t start = 0, length = 0;
    Hash sha{};
};
struct TocRec {
    uint8_t track = 0, control = 0, adr = 1, session = 1;
    int32_t lba = 0;
    uint8_t stored = 1;
};

struct Layout {
    uint16_t flags = 0;
    uint32_t rate = 44100;
    uint8_t channels = 2, bps = 16;
    uint8_t first_track = 1, last_track = 1;
    int32_t start_lba = 0;
    uint64_t total = 0;
    Hash timeline_sha{};
    std::string mcn;
    std::vector<TrackRec> tracks;
    std::vector<IndexRec> indexes;
    std::vector<SegmentRec> segments;
    std::vector<TocRec> toc;

    uint32_t bytesPerSample() const { return (bps + 7) / 8; }
    uint64_t pcmBytes() const { return total * channels * bytesPerSample(); }
    bool cdda() const { return flags & LF_CDDA; }
    const IndexRec *findIndex(uint8_t track, uint8_t idx) const;
};
std::vector<uint8_t> serializeLayout(const Layout &l);
Layout parseLayout(const uint8_t *p, size_t n);  // structural validation
bool computeCddaFlag(const Layout &l);
std::string musicbrainzDiscId(const Layout &l, std::string *toc_string);

struct Picture {
    uint32_t type = 3;
    std::string mime, desc;
    uint32_t width = 0, height = 0, depth = 0, colors = 0;
    std::vector<uint8_t> data;
};
std::vector<uint8_t> serializePicture(const Picture &p);
Picture parsePicture(const uint8_t *p, size_t n);
Picture pictureFromFile(const std::string &path, uint32_t type);  // sniffs & validates
const char *pictureTypeName(uint32_t t);

json normalizeMeta(const json &in, const Layout &l, bool fill_provenance);
void validateMeta(const json &m, const Layout &l);  // throws FISO_E_FORMAT

// ---------------------------------------------------------------- canonical pcm
// Appends canonical little-endian bytes of `frames` interleaved samples.
void canonicalBytes(const int32_t *s, size_t samples, uint32_t bps, std::vector<uint8_t> &out);

// ---------------------------------------------------------------- progress
struct Progress {
    fiso_progress_fn cb = nullptr;
    void *user = nullptr;
    void operator()(const char *phase, uint64_t done, uint64_t total) const {
        if (cb && cb(user, phase, done, total)) fail(FISO_E_CANCELLED, "operation cancelled");
    }
};

// ---------------------------------------------------------------- reading
struct ChunkEntry {
    std::string type;
    uint32_t flags = 0;
    uint64_t offset = 0, length = 0;
    Hash sha{};
    uint32_t version = 1;
};

class SegmentDecoder;

class Image {
  public:
    static std::unique_ptr<Image> open(const std::string &path);
    ~Image();

    std::string path;
    uint16_t vmajor = 0, vminor = 0;
    uint64_t file_size = 0;
    std::vector<ChunkEntry> chunks;
    Layout layout;
    json meta;
    std::string meta_raw;
    std::vector<size_t> pict_chunks;
    std::vector<std::string> warnings;

    std::vector<uint8_t> readChunk(size_t idx, uint64_t max);
    Picture readPicture(size_t chunk_idx);
    const SegmentRec *segmentForTrack(uint8_t number) const;

    // Random-access PCM (interleaved int32)
    size_t readPcm(uint64_t start, size_t frames, int32_t *out);
    File &file() { return file_; }

  private:
    Image() = default;
    File file_;
    std::vector<std::unique_ptr<SegmentDecoder>> decoders_;
    friend class SegmentDecoder;
};

// Decodes a FLAC stream that lives in [base, base+len) of an open file.
class SegmentDecoder {
  public:
    SegmentDecoder(File &f, uint64_t base, uint64_t len);
    ~SegmentDecoder();
    void seek(uint64_t sample);
    size_t read(int32_t *out, size_t frames);  // returns < frames only at end
    uint32_t rate() const { return rate_; }
    uint32_t channels() const { return ch_; }
    uint32_t bps() const { return bps_; }
    uint64_t totalSamples() const { return total_; }
    const uint8_t *md5() const { return md5_; }
    bool md5Ok();  // only meaningful after decoding the whole stream from 0
    // internal state used by callbacks
    File &f_;
    uint64_t base_, len_, pos_ = 0;
    std::vector<int32_t> buf_;
    size_t bufpos_ = 0;
    uint32_t rate_ = 0, ch_ = 0, bps_ = 0;
    uint64_t total_ = 0;
    uint8_t md5_[16]{};
    bool error_ = false;
    std::string errmsg_;
    uint64_t next_ = 0;  // next sample read() will return
  private:
    void *dec_ = nullptr;
    bool fill();
};

// ---------------------------------------------------------------- writing
class ContainerWriter {
  public:
    ContainerWriter(const std::string &target, bool overwrite);
    ~ContainerWriter();
    size_t begin(const char *type, uint32_t flags);  // returns chunk index
    void end();                                      // closes current chunk, hashes it
    size_t addChunk(const char *type, uint32_t flags, const std::vector<uint8_t> &data);
    size_t copyChunk(Image &src, size_t idx, const Progress &prog);
    File &file() { return file_; }
    uint64_t chunkBase() const { return cur_base_; }
    void finish();  // table, header, fsync, atomic rename
    const std::vector<ChunkEntry> &entries() const { return entries_; }
    uint64_t finalSize() const { return final_size_; }

  private:
    std::string target_, tmp_;
    bool overwrite_;
    File file_;
    std::vector<ChunkEntry> entries_;
    bool open_chunk_ = false, finished_ = false;
    uint64_t cur_base_ = 0, final_size_ = 0;
    void align();
};

// ---------------------------------------------------------------- sources
class PcmSource {
  public:
    virtual ~PcmSource() = default;
    uint32_t rate = 0, channels = 0, bps = 0;
    uint64_t total = 0;  // frames
    virtual void seek(uint64_t frame) = 0;
    virtual size_t read(int32_t *out, size_t frames) = 0;
};
std::unique_ptr<PcmSource> openWav(const std::string &path);
std::unique_ptr<PcmSource> openRawCdda(const std::string &path, bool big_endian);
std::unique_ptr<PcmSource> openFlacFile(const std::string &path);
std::unique_ptr<PcmSource> openAnyAudio(const std::string &path);  // by content sniffing

// ---------------------------------------------------------------- operations
json createImage(const json &plan, const Progress &prog);
json verifyImage(const std::string &path, bool deep, const Progress &prog);
void editImage(const std::string &path, const json &edit, const std::string &out, const Progress &prog);
json extractImage(const std::string &path, const json &opts, const Progress &prog);
json groupImages(const std::vector<std::string> &paths);
json infoJson(Image &img);
std::string buildCue(Image &img, const std::string &bin_name);
std::vector<uint8_t> flacWithTags(Image &img, const SegmentRec &seg, bool with_picture);

}  // namespace fiso
