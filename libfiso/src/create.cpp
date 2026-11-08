// Image creation (files, CUE sheets, raw PCM with explicit layout) — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <cstring>
#include <sstream>

namespace fiso {

namespace {

enum SrcKind { SK_ANY, SK_RAW_LE, SK_RAW_BE, SK_FISO };

struct Src {
    std::string path;
    SrcKind kind;
    uint64_t len;
    bool is_flac = false;
    uint32_t rate = 0, ch = 0, bps = 0;
};
struct Piece {
    int src = -1;
    uint64_t src_start = 0, length = 0;
};  // src -1 = digital silence
struct PTrack {
    int number = 0;
    uint8_t flags = 0;
    std::string isrc, title, performer, songwriter;
    std::vector<std::pair<int, uint64_t>> idx;  // (index number, timeline position)
    int passthrough_src = -1;
    uint64_t pregap = 0, postgap = 0;  // CUE PREGAP/POSTGAP, samples
};
struct Timeline {
    uint32_t rate = 0, ch = 0, bps = 0;
    std::vector<Src> srcs;
    std::vector<Piece> pieces;
    std::vector<PTrack> tracks;
    std::string mcn;
    bool synth_silence = false, physical = false;
    int32_t start_lba = 0;
    std::vector<TocRec> toc;
    json hint_meta = json::object();  // metadata found in the source (CUE)
    std::string kind;
    std::vector<std::string> warnings;
    std::vector<Picture> pictures;  // pictures inherited from a FISO source
    json mastering;                 // CD mastering report (alignment, conversion)
    uint64_t total() const {
        uint64_t t = 0;
        for (auto &p : pieces) t += p.length;
        return t;
    }
};

// The whole timeline of an existing FISO image as a PCM source.
class FisoSource : public PcmSource {
  public:
    explicit FisoSource(const std::string &path) : img_(Image::open(path)) {
        rate = img_->layout.rate;
        channels = img_->layout.channels;
        bps = img_->layout.bps;
        total = img_->layout.total;
    }
    void seek(uint64_t f) override { pos_ = f; }
    size_t read(int32_t *out, size_t n) override {
        size_t k = img_->readPcm(pos_, n, out);
        pos_ += k;
        return k;
    }
    Image &image() { return *img_; }

  private:
    std::unique_ptr<Image> img_;
    uint64_t pos_ = 0;
};

std::unique_ptr<PcmSource> openSrc(const Src &s) {
    if (s.kind == SK_FISO) return std::make_unique<FisoSource>(s.path);
    if (s.kind == SK_RAW_LE) return openRawCdda(s.path, false);
    if (s.kind == SK_RAW_BE) return openRawCdda(s.path, true);
    return openAnyAudio(s.path);
}

int addSource(Timeline &tl, const std::string &path, SrcKind kind) {
    if (!pathExists(path)) fail(FISO_E_IO, "source file not found: '" + path + "'");
    Src s{path, kind, 0};
    auto p = openSrc(s);
    s.rate = p->rate;
    s.ch = p->channels;
    s.bps = p->bps;
    if (tl.srcs.empty()) {
        tl.rate = p->rate;
        tl.ch = p->channels;
        tl.bps = p->bps;
    } else if (p->rate != tl.rate || p->channels != tl.ch || p->bps != tl.bps)
        fail(FISO_E_UNSUPPORTED, "'" + path + "' has a different audio format than the previous files (" +
                                     std::to_string(p->rate) + " Hz/" + std::to_string(p->bps) + " bit/" +
                                     std::to_string(p->channels) + " ch). FISO never converts audio silently.");
    s.len = p->total;
    {
        File f = File::open(path, "rb");
        uint8_t h[4] = {0};
        f.readSome(h, 4);
        s.is_flac = kind == SK_ANY && memcmp(h, "fLaC", 4) == 0;
    }
    tl.srcs.push_back(s);
    return (int)tl.srcs.size() - 1;
}

// ---------------------------------------------------------------- reader across pieces
class TimelineReader {
  public:
    explicit TimelineReader(const Timeline &t) : t_(t) {}
    void seekTo(uint64_t pos) {
        uint64_t acc = 0;
        for (pi_ = 0; pi_ < t_.pieces.size(); pi_++) {
            if (pos < acc + t_.pieces[pi_].length) break;
            acc += t_.pieces[pi_].length;
        }
        off_ = pos - acc;
        need_seek_ = true;
    }
    size_t read(int32_t *out, size_t frames) {
        size_t done = 0;
        while (done < frames && pi_ < t_.pieces.size()) {
            const Piece &p = t_.pieces[pi_];
            size_t n = (size_t)std::min<uint64_t>(frames - done, p.length - off_);
            if (p.src < 0) {
                memset(out + done * t_.ch, 0, n * t_.ch * sizeof(int32_t));
            } else {
                if (p.src != cur_src_) {
                    cur_ = openSrc(t_.srcs[p.src]);
                    cur_src_ = p.src;
                    need_seek_ = true;
                }
                if (need_seek_) {
                    cur_->seek(p.src_start + off_);
                    need_seek_ = false;
                }
                size_t got = cur_->read(out + done * t_.ch, n);
                if (got != n) fail(FISO_E_FORMAT, "source '" + t_.srcs[p.src].path + "' ended early");
            }
            done += n;
            off_ += n;
            if (off_ == p.length) {
                pi_++;
                off_ = 0;
                need_seek_ = true;
            }
        }
        return done;
    }

  private:
    const Timeline &t_;
    size_t pi_ = 0;
    uint64_t off_ = 0;
    std::unique_ptr<PcmSource> cur_;
    int cur_src_ = -1;
    bool need_seek_ = true;
};

// ---------------------------------------------------------------- CUE
std::string toUtf8(const std::string &s) {
    // valid UTF-8 is returned unchanged, anything else is read as Latin-1
    bool ok = true;
    for (size_t i = 0; i < s.size() && ok;) {
        unsigned char c = s[i];
        size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99 || i + n >= s.size() + (n ? 0 : 1)) {
            ok = false;
            break;
        }
        for (size_t k = 1; k <= n; k++)
            if (((unsigned char)s[i + k] >> 6) != 2) ok = false;
        i += n + 1;
    }
    if (ok) return s;
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80)
            o.push_back((char)c);
        else {
            o.push_back((char)(0xC0 | (c >> 6)));
            o.push_back((char)(0x80 | (c & 0x3f)));
        }
    }
    return o;
}

std::vector<std::string> cueTokens(const std::string &line) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isspace((unsigned char)line[i])) i++;
        if (i >= line.size()) break;
        std::string w;
        if (line[i] == '"') {
            i++;
            while (i < line.size() && line[i] != '"') w.push_back(line[i++]);
            i++;
        } else {
            while (i < line.size() && !isspace((unsigned char)line[i])) w.push_back(line[i++]);
        }
        t.push_back(w);
    }
    return t;
}

uint64_t parseMsf(const std::string &s, uint32_t rate) {
    unsigned m, sec, f;
    char extra;
    if (sscanf(s.c_str(), "%u:%u:%u%c", &m, &sec, &f, &extra) != 3 || sec > 59 || f > 74)
        fail(FISO_E_FORMAT, "CUE: invalid time '" + s + "'");
    uint64_t frames = ((uint64_t)m * 60 + sec) * 75 + f;
    if ((frames * rate) % 75) fail(FISO_E_UNSUPPORTED, "CUE: sample rate incompatible with CD frames");
    return frames * rate / 75;
}

std::string resolveCueFile(const std::string &cue_dir, const std::string &name) {
    std::string p = joinPath(cue_dir, name);
    if (pathExists(p)) return p;
    // common case: the CUE names a .wav that has since been converted to FLAC
    auto dot = name.rfind('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    for (const char *ext : {".flac", ".wav", ".bin", ".FLAC", ".WAV", ".BIN"}) {
        std::string q = joinPath(cue_dir, stem + ext);
        if (pathExists(q)) return q;
    }
    fail(FISO_E_IO, "CUE: referenced file not found: '" + name + "'");
}

void buildFromCue(Timeline &tl, const std::string &cue_path) {
    auto raw = readWholeFile(cue_path, 4 << 20);
    std::string text(raw.begin(), raw.end());
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    text = toUtf8(text);
    std::string dir = dirName(cue_path);
    tl.kind = "cue";

    json &hm = tl.hint_meta;
    hm["album"] = json::object();
    hm["cdtext"] = json::object();

    int cur_file = -1;
    uint64_t file_base = 0, cursor = 0, shift = 0;
    PTrack *cur = nullptr;
    uint64_t pending_postgap = 0;
    bool first_index_of_track = false;

    auto timelineEnd = [&]() { return tl.total(); };
    auto emitFileUpTo = [&](uint64_t p) {
        if (cur_file < 0) fail(FISO_E_FORMAT, "CUE: INDEX before FILE");
        if (p < cursor || p > tl.srcs[cur_file].len) fail(FISO_E_FORMAT, "CUE: INDEX position outside its file");
        if (p > cursor) tl.pieces.push_back({cur_file, cursor, p - cursor});
        cursor = p;
    };
    auto emitSilence = [&](uint64_t n) {
        if (!n) return;
        tl.pieces.push_back({-1, 0, n});
        shift += n;
        tl.synth_silence = true;
    };
    auto flushFile = [&]() {
        if (cur_file >= 0) emitFileUpTo(tl.srcs[cur_file].len);
    };

    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        auto t = cueTokens(line);
        if (t.empty()) continue;
        std::string cmd = t[0];
        for (auto &c : cmd) c = (char)toupper((unsigned char)c);
        auto arg = [&](size_t i) -> std::string {
            if (i >= t.size()) fail(FISO_E_FORMAT, "CUE line " + std::to_string(lineno) + ": missing argument");
            return t[i];
        };
        if (cmd == "REM") {
            if (t.size() < 3) continue;
            std::string k = t[1];
            for (auto &c : k) c = (char)toupper((unsigned char)c);
            std::string v = t[2];
            for (size_t i = 3; i < t.size(); i++) v += " " + t[i];
            if (cur) continue;
            if (k == "GENRE")
                hm["album"]["genre"] = v;
            else if (k == "DATE")
                hm["album"]["date"] = v;
            else if (k == "DISCNUMBER")
                hm["disc"]["number"] = atoi(v.c_str());
            else if (k == "TOTALDISCS" || k == "DISCTOTAL")
                hm["disc"]["total"] = atoi(v.c_str());
            else if (k == "COMMENT")
                hm["tags"]["COMMENT"] = v;
        } else if (cmd == "CATALOG") {
            tl.mcn = arg(1);
        } else if (cmd == "TITLE" || cmd == "PERFORMER" || cmd == "SONGWRITER") {
            std::string v = arg(1);
            if (!cur) {
                if (cmd == "TITLE") {
                    hm["album"]["title"] = v;
                    hm["cdtext"]["album_title"] = v;
                }
                if (cmd == "PERFORMER") {
                    hm["album"]["artist"] = v;
                    hm["cdtext"]["album_performer"] = v;
                }
            } else {
                if (cmd == "TITLE") cur->title = v;
                if (cmd == "PERFORMER") cur->performer = v;
                if (cmd == "SONGWRITER") cur->songwriter = v;
            }
        } else if (cmd == "FILE") {
            std::string name = arg(1), type = t.size() > 2 ? t[2] : "WAVE";
            for (auto &c : type) c = (char)toupper((unsigned char)c);
            if (type == "MP3" || type == "AIFF") fail(FISO_E_UNSUPPORTED, "CUE: " + type + " files are not supported");
            flushFile();
            std::string path = resolveCueFile(dir, name);
            SrcKind k = type == "BINARY" ? SK_RAW_LE : type == "MOTOROLA" ? SK_RAW_BE : SK_ANY;
            // a .bin declared as WAVE is still raw PCM
            if (k == SK_ANY) {
                File f = File::open(path, "rb");
                uint8_t h[4] = {0};
                f.readSome(h, 4);
                if (memcmp(h, "RIFF", 4) && memcmp(h, "fLaC", 4) && memcmp(h, "ID3", 3)) k = SK_RAW_LE;
            }
            cur_file = addSource(tl, path, k);
            file_base = timelineEnd();
            cursor = 0;
            shift = 0;
        } else if (cmd == "TRACK") {
            int n = atoi(arg(1).c_str());
            std::string type = arg(2);
            for (auto &c : type) c = (char)toupper((unsigned char)c);
            if (type != "AUDIO")
                fail(FISO_E_UNSUPPORTED, "CUE: track " + std::to_string(n) + " is a data track (" + type +
                                             "). FISO v1.0 stores audio only; mixed-mode discs are not supported.");
            if (n < 1 || n > 99 || (!tl.tracks.empty() && n != tl.tracks.back().number + 1))
                fail(FISO_E_FORMAT, "CUE: track numbers must be consecutive (1-99)");
            if (cur && cur->idx.empty()) fail(FISO_E_FORMAT, "CUE: track without INDEX");
            if (cur) pending_postgap = cur->postgap;
            tl.tracks.push_back(PTrack());
            cur = &tl.tracks.back();
            cur->number = n;
            first_index_of_track = true;
        } else if (cmd == "FLAGS") {
            if (!cur) continue;
            for (size_t i = 1; i < t.size(); i++) {
                std::string f = t[i];
                for (auto &c : f) c = (char)toupper((unsigned char)c);
                if (f == "PRE")
                    cur->flags |= 1;
                else if (f == "DCP")
                    cur->flags |= 2;
                else if (f == "4CH")
                    cur->flags |= 4;
            }
        } else if (cmd == "ISRC") {
            if (cur) cur->isrc = arg(1);
        } else if (cmd == "PREGAP") {
            if (!cur) fail(FISO_E_FORMAT, "CUE: PREGAP outside a track");
            cur->pregap = parseMsf(arg(1), tl.rate ? tl.rate : 44100);
        } else if (cmd == "POSTGAP") {
            if (!cur) fail(FISO_E_FORMAT, "CUE: POSTGAP outside a track");
            cur->postgap = parseMsf(arg(1), tl.rate ? tl.rate : 44100);
        } else if (cmd == "INDEX") {
            if (!cur || cur_file < 0) fail(FISO_E_FORMAT, "CUE: INDEX before TRACK/FILE");
            int num = atoi(arg(1).c_str());
            uint64_t p = parseMsf(arg(2), tl.rate);
            if (first_index_of_track && pending_postgap) {
                emitFileUpTo(p);
                emitSilence(pending_postgap);
                pending_postgap = 0;
            }
            first_index_of_track = false;
            if (num == 1 && cur->pregap) {
                emitFileUpTo(p);
                bool has00 = false;
                for (auto &x : cur->idx)
                    if (x.first == 0) has00 = true;
                if (!has00) cur->idx.push_back({0, timelineEnd()});
                emitSilence(cur->pregap);
                cur->pregap = 0;
            }
            cur->idx.push_back({num, file_base + p + shift});
        } else if (cmd == "CDTEXTFILE") {
            tl.warnings.push_back("CDTEXTFILE ignored");
        }
    }
    if (!cur) fail(FISO_E_FORMAT, "CUE: no tracks");
    if (cur->idx.empty()) fail(FISO_E_FORMAT, "CUE: last track has no INDEX");
    flushFile();
    if (cur->postgap) emitSilence(cur->postgap);

    json tracks = json::array();
    for (auto &tr : tl.tracks) {
        json tj = {{"number", tr.number}};
        if (!tr.title.empty()) tj["title"] = tr.title;
        if (!tr.performer.empty()) tj["artist"] = tr.performer;
        tracks.push_back(tj);
    }
    hm["tracks"] = tracks;
    if (hm["album"].empty()) hm.erase("album");
}

void buildFromFiles(Timeline &tl, const json &files) {
    tl.kind = "files";
    if (!files.is_array() || files.empty() || files.size() > 99) fail(FISO_E_ARG, "files: 1 to 99 audio files expected");
    int n = 1;
    for (auto &f : files) {
        int s = addSource(tl, f.get<std::string>(), SK_ANY);
        PTrack t;
        t.number = n++;
        t.idx.push_back({1, tl.total()});
        if (tl.srcs[s].is_flac) t.passthrough_src = s;
        tl.pieces.push_back({s, 0, tl.srcs[s].len});
        tl.tracks.push_back(t);
    }
}

// Raw PCM with an explicit layout — used by disc rippers and conformance tests.
void buildFromRaw(Timeline &tl, const json &src) {
    tl.kind = src.value("provenance_source", std::string("cd"));
    int s = addSource(tl, src.at("path").get<std::string>(), src.value("big_endian", false) ? SK_RAW_BE : SK_RAW_LE);
    tl.pieces.push_back({s, 0, tl.srcs[s].len});
    tl.physical = src.value("physical", false);
    tl.start_lba = src.value("start_lba", 0);
    tl.mcn = src.value("mcn", std::string());
    for (auto &tj : src.at("tracks")) {
        PTrack t;
        t.number = tj.at("number");
        t.flags = tj.value("flags", 0);
        t.isrc = tj.value("isrc", std::string());
        for (auto &ix : tj.at("indexes")) t.idx.push_back({ix.at(0).get<int>(), ix.at(1).get<uint64_t>()});
        tl.tracks.push_back(t);
    }
    if (src.contains("toc"))
        for (auto &e : src["toc"]) {
            TocRec r;
            r.track = e.at("track");
            r.control = e.value("control", 0);
            r.adr = e.value("adr", 1);
            r.session = e.value("session", 1);
            r.lba = e.at("lba");
            r.stored = e.value("stored", 1);
            tl.toc.push_back(r);
        }
}

// ---------------------------------------------------------------- FLAC encode into the container
struct EncCtx {
    File *f;
    uint64_t base, pos;
};
FLAC__StreamEncoderWriteStatus eWrite(const FLAC__StreamEncoder *, const FLAC__byte buf[], size_t n, uint32_t, uint32_t,
                                      void *u) {
    auto *c = (EncCtx *)u;
    try {
        c->f->seek(c->base + c->pos);
        c->f->write(buf, n);
    } catch (...) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    c->pos += n;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
FLAC__StreamEncoderSeekStatus eSeek(const FLAC__StreamEncoder *, FLAC__uint64 off, void *u) {
    ((EncCtx *)u)->pos = off;
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}
FLAC__StreamEncoderTellStatus eTell(const FLAC__StreamEncoder *, FLAC__uint64 *off, void *u) {
    *off = ((EncCtx *)u)->pos;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

struct EncoderGuard {
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__StreamMetadata *seek = nullptr;
    ~EncoderGuard() {
        if (e) {
            FLAC__stream_encoder_finish(e);
            FLAC__stream_encoder_delete(e);
        }
        if (seek) FLAC__metadata_object_delete(seek);
    }
};

void encodeSegment(ContainerWriter &w, const Timeline &tl, uint64_t start, uint64_t len, int level, Hash &seg_sha,
                   Sha256 &timeline_sha, const Progress &prog, uint64_t &done, uint64_t all) {
    w.begin("FLAC", kChunkCritical);
    EncoderGuard g;
    if (!g.e) fail(FISO_E_INTERNAL, "out of memory");
    FLAC__stream_encoder_set_verify(g.e, true);
    FLAC__stream_encoder_set_compression_level(g.e, (uint32_t)level);
    FLAC__stream_encoder_set_channels(g.e, tl.ch);
    FLAC__stream_encoder_set_bits_per_sample(g.e, tl.bps);
    FLAC__stream_encoder_set_sample_rate(g.e, tl.rate);
    FLAC__stream_encoder_set_streamable_subset(g.e, tl.bps <= 24 && tl.rate <= 655350);
    FLAC__stream_encoder_set_total_samples_estimate(g.e, len);
    g.seek = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
    if (!g.seek || !FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(g.seek, tl.rate * 10, len) ||
        !FLAC__metadata_object_seektable_template_sort(g.seek, true))
        fail(FISO_E_INTERNAL, "cannot build FLAC seek table");
    FLAC__StreamMetadata *md[1] = {g.seek};
    FLAC__stream_encoder_set_metadata(g.e, md, 1);
    EncCtx ctx{&w.file(), w.chunkBase(), 0};
    if (FLAC__stream_encoder_init_stream(g.e, eWrite, eSeek, eTell, nullptr, &ctx) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        fail(FISO_E_FLAC, "cannot initialise FLAC encoder");

    TimelineReader rd(tl);
    rd.seekTo(start);
    const size_t block = 16384;
    std::vector<int32_t> buf(block * tl.ch);
    std::vector<uint8_t> bytes;
    Sha256 sh;
    uint64_t left = len;
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, block);
        if (rd.read(buf.data(), n) != n) fail(FISO_E_FORMAT, "source audio ended early");
        bytes.clear();
        canonicalBytes(buf.data(), n * tl.ch, tl.bps, bytes);
        sh.update(bytes.data(), bytes.size());
        timeline_sha.update(bytes.data(), bytes.size());
        if (!FLAC__stream_encoder_process_interleaved(g.e, buf.data(), (uint32_t)n)) {
            auto st = FLAC__stream_encoder_get_state(g.e);
            fail(st == FLAC__STREAM_ENCODER_VERIFY_MISMATCH_IN_AUDIO_DATA ? FISO_E_INTEGRITY : FISO_E_FLAC,
                 std::string("FLAC encoder: ") + FLAC__StreamEncoderStateString[st]);
        }
        left -= n;
        done += n;
        prog("encode", done, all);
    }
    bool ok = FLAC__stream_encoder_finish(g.e);
    auto st = FLAC__stream_encoder_get_state(g.e);
    FLAC__stream_encoder_delete(g.e);
    g.e = nullptr;
    if (!ok) fail(FISO_E_INTEGRITY, std::string("FLAC encoder verification failed: ") + FLAC__StreamEncoderStateString[st]);
    seg_sha = sh.finish();
    w.end();
}

// Copies an existing FLAC file's frames without re-encoding. Returns false if not eligible.
bool passthroughSegment(ContainerWriter &w, const Src &s, const Timeline &tl, Hash &seg_sha, Sha256 &timeline_sha,
                        const Progress &prog, uint64_t &done, uint64_t all) {
    File f = File::open(s.path, "rb");
    uint64_t fsize = f.size();
    uint8_t h[4];
    f.read(h, 4);
    if (memcmp(h, "fLaC", 4) != 0) return false;
    std::vector<uint8_t> si, st;
    uint64_t pos = 4;
    bool last = false;
    while (!last) {
        uint8_t bh[4];
        f.seek(pos);
        f.read(bh, 4);
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t len = (uint32_t)bh[1] << 16 | bh[2] << 8 | bh[3];
        if (pos + 4 + len > fsize) return false;
        std::vector<uint8_t> body(len);
        f.read(body.data(), len);
        if (type == 0)
            si = body;
        else if (type == 3)
            st = body;
        else if (type == 127)
            return false;
        pos += 4 + len;
    }
    if (si.size() != 34) return false;
    static const uint8_t zero[16] = {0};
    if (memcmp(si.data() + 18, zero, 16) == 0) return false;  // no MD5 in STREAMINFO
    uint64_t frames_off = pos;

    w.begin("FLAC", kChunkCritical);
    File &o = w.file();
    o.write("fLaC", 4);
    auto hdr = [&](int type, bool islast, size_t len) {
        uint8_t b[4] = {(uint8_t)((islast ? 0x80 : 0) | type), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        o.write(b, 4);
    };
    hdr(0, st.empty(), si.size());
    o.write(si.data(), si.size());
    if (!st.empty()) {
        hdr(3, true, st.size());
        o.write(st.data(), st.size());
    }
    std::vector<uint8_t> buf(1 << 20);
    uint64_t left = fsize - frames_off;
    f.seek(frames_off);
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, buf.size());
        f.read(buf.data(), n);
        o.write(buf.data(), n);
        left -= n;
    }
    w.end();

    // hash the decoded PCM (this also proves the copied stream decodes)
    auto src = openFlacFile(s.path);
    std::vector<int32_t> pcm(16384 * tl.ch);
    std::vector<uint8_t> bytes;
    Sha256 sh;
    uint64_t rem = src->total;
    while (rem) {
        size_t n = (size_t)std::min<uint64_t>(rem, 16384);
        if (src->read(pcm.data(), n) != n) fail(FISO_E_FORMAT, "FLAC source ended early");
        bytes.clear();
        canonicalBytes(pcm.data(), n * tl.ch, tl.bps, bytes);
        sh.update(bytes.data(), bytes.size());
        timeline_sha.update(bytes.data(), bytes.size());
        rem -= n;
        done += n;
        prog("encode", done, all);
    }
    seg_sha = sh.finish();
    return true;
}

json mergeMeta(json base, const json &over) {
    if (!over.is_object()) return base;
    for (auto &[k, v] : over.items()) {
        if (k == "tracks" && v.is_array()) {
            if (!base.contains("tracks") || !base["tracks"].is_array()) base["tracks"] = json::array();
            for (auto &t : v) {
                if (!t.is_object() || !t.contains("number")) continue;
                bool found = false;
                for (auto &b : base["tracks"])
                    if (b.value("number", -1) == t["number"].get<int>()) {
                        b = mergeMeta(b, t);
                        found = true;
                    }
                if (!found) base["tracks"].push_back(t);
            }
        } else if (v.is_object() && base.contains(k) && base[k].is_object()) {
            base[k] = mergeMeta(base[k], v);
        } else {
            base[k] = v;
        }
    }
    return base;
}

void buildFromFiso(Timeline &tl, const std::string &path) {
    tl.kind = "fiso";
    int si = addSource(tl, path, SK_FISO);
    FisoSource fs(path);
    Image &img = fs.image();
    const Layout &L = img.layout;
    tl.pieces.push_back({si, 0, L.total});
    for (auto &t : L.tracks) {
        PTrack pt;
        pt.number = t.number;
        pt.flags = t.flags;
        pt.isrc = t.isrc;
        for (int k = 0; k < t.index_count; k++)
            pt.idx.push_back({L.indexes[t.first_index + k].index, L.indexes[t.first_index + k].pos});
        tl.tracks.push_back(pt);
    }
    tl.mcn = L.mcn;
    tl.synth_silence = L.flags & LF_SYNTH_SILENCE;
    json m = img.meta;
    m.erase("provenance");
    tl.hint_meta = m;
    for (size_t ci : img.pict_chunks) tl.pictures.push_back(img.readPicture(ci));
}

// ---------------------------------------------------------------- CD mastering
// Converts one source to raw 44.1 kHz / 16-bit / stereo PCM. Resampling uses a
// Kaiser-windowed sinc filter; requantisation uses TPDF dither. Exact (bit-identical)
// when the source is already 16-bit at 44.1 kHz.
uint64_t convertToCd(const Src &s, const std::string &out, const Progress &prog, uint64_t &done, uint64_t all) {
    if (s.ch > 2)
        fail(FISO_E_UNSUPPORTED, "[multichannel] '" + s.path + "' has " + std::to_string(s.ch) +
                                     " channels; an audio CD is stereo and FISO does not downmix");
    auto src = openSrc(s);
    File f = File::open(out, "wb");
    const double ratio = 44100.0 / s.rate;
    const uint64_t outFrames = (uint64_t)std::llround((double)s.len * ratio);
    const bool resample = s.rate != 44100;
    const bool lossy = resample || s.bps > 16;
    const double scale = 1.0 / (double)(1u << (s.bps - 1));
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (double)(rng >> 11) / 9007199254740992.0;
    };
    std::vector<uint8_t> bytes;
    auto emit = [&](double l, double r) {
        int32_t v[2];
        double c[2] = {l, r};
        for (int k = 0; k < 2; k++) {
            double x = c[k] * 32768.0;
            if (lossy) x += rnd() - rnd();  // triangular dither, +/- 1 LSB
            long q = std::lround(x);
            v[k] = (int32_t)std::clamp<long>(q, -32768, 32767);
        }
        bytes.push_back((uint8_t)v[0]);
        bytes.push_back((uint8_t)(v[0] >> 8));
        bytes.push_back((uint8_t)v[1]);
        bytes.push_back((uint8_t)(v[1] >> 8));
    };
    std::vector<int32_t> in(16384 * s.ch);
    if (!resample) {
        uint64_t left = s.len;
        while (left) {
            size_t n = (size_t)std::min<uint64_t>(left, 16384);
            if (src->read(in.data(), n) != n) fail(FISO_E_FORMAT, "source '" + s.path + "' ended early");
            bytes.clear();
            for (size_t i = 0; i < n; i++) {
                int32_t a = in[i * s.ch], b = s.ch == 2 ? in[i * s.ch + 1] : a;
                if (!lossy) {  // exact: 8..16-bit integer samples widened to 16-bit
                    int sh = 16 - (int)s.bps;
                    int32_t x = a * (1 << sh), y = b * (1 << sh);
                    bytes.push_back((uint8_t)x);
                    bytes.push_back((uint8_t)(x >> 8));
                    bytes.push_back((uint8_t)y);
                    bytes.push_back((uint8_t)(y >> 8));
                } else
                    emit(a * scale, b * scale);
            }
            f.write(bytes.data(), bytes.size());
            left -= n;
            done += n;
            prog("convert", done, all);
        }
    } else {
        const double fc = 0.5 * std::min(1.0, ratio) * 0.95;  // cut-off, cycles per input sample
        const int Z = 20;                                     // zero crossings on each side
        const double hw = Z / (2.0 * fc);                     // half-width in input samples
        const int P = 512;                                    // table resolution per input sample
        const int HW = (int)std::ceil(hw);
        std::vector<double> table((size_t)(HW + 1) * P + 2);
        auto bessel0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 40; k++) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        const double beta = 9.0, i0b = bessel0(beta);
        for (size_t i = 0; i < table.size(); i++) {
            double tau = (double)i / P;
            double w = tau >= hw ? 0.0 : bessel0(beta * std::sqrt(1 - (tau / hw) * (tau / hw))) / i0b;
            double x = 2 * fc * tau;
            double sinc = x == 0 ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
            table[i] = 2 * fc * sinc * w;
        }
        auto h = [&](double tau) {
            tau = std::fabs(tau) * P;
            size_t i = (size_t)tau;
            if (i + 1 >= table.size()) return 0.0;
            double fr = tau - (double)i;
            return table[i] + (table[i + 1] - table[i]) * fr;
        };
        std::vector<double> buf[2];
        int64_t base = 0;  // input index of buf[...][0]
        uint64_t read_in = 0;
        bool eof = false;
        const double step = 1.0 / ratio;
        for (uint64_t n = 0; n < outFrames;) {
            double t = (double)n * step;
            int64_t center = (int64_t)std::floor(t);
            int64_t need_hi = center + HW + 1;
            while (!eof && (int64_t)read_in <= need_hi) {
                size_t k = (size_t)std::min<uint64_t>(16384, s.len - read_in);
                if (k == 0) {
                    eof = true;
                    break;
                }
                if (src->read(in.data(), k) != k) fail(FISO_E_FORMAT, "source '" + s.path + "' ended early");
                for (size_t i = 0; i < k; i++) {
                    double a = in[i * s.ch] * scale;
                    buf[0].push_back(a);
                    buf[1].push_back(s.ch == 2 ? in[i * s.ch + 1] * scale : a);
                }
                read_in += k;
                done += k;
                prog("convert", done, all);
            }
            // drop input that no future output needs
            int64_t keep_from = center - HW - 1;
            if (keep_from - base > 65536) {
                size_t drop = (size_t)(keep_from - base);
                buf[0].erase(buf[0].begin(), buf[0].begin() + drop);
                buf[1].erase(buf[1].begin(), buf[1].begin() + drop);
                base += (int64_t)drop;
            }
            bytes.clear();
            uint64_t batch_end = std::min<uint64_t>(outFrames, n + 4096);
            for (; n < batch_end; n++) {
                t = (double)n * step;
                center = (int64_t)std::floor(t);
                if (!eof && center + HW + 1 >= (int64_t)read_in) break;
                double acc[2] = {0, 0};
                for (int64_t k = center - HW; k <= center + HW + 1; k++) {
                    if (k < 0 || k >= (int64_t)read_in) continue;
                    double c = h(t - (double)k);
                    acc[0] += c * buf[0][(size_t)(k - base)];
                    acc[1] += c * buf[1][(size_t)(k - base)];
                }
                emit(acc[0], acc[1]);
            }
            f.write(bytes.data(), bytes.size());
        }
    }
    f.sync();
    return outFrames;
}

std::string fmtName(uint32_t rate, uint32_t bps, uint32_t ch) {
    char b[64];
    snprintf(b, sizeof b, "%.1f kHz / %u bit / %s", rate / 1000.0, bps,
             ch == 1   ? "mono"
             : ch == 2 ? "stereo"
                       : (std::to_string(ch) + " ch").c_str());
    return b;
}

// Turns the timeline of an existing image into a CD-DA one (see spec, provenance.cd_mastering):
// converts the audio when explicitly allowed, aligns indexes on 1/75 s sectors and pads the end.
void masterForCd(Timeline &tl, const json &plan, const std::string &out, std::vector<std::string> &temps, const Progress &prog) {
    bool allow_conv = plan.value("allow_conversion", false);
    json rep = {{"alignment", "shift"}};

    // 1) audio format
    bool need_conv = false;
    for (auto &s : tl.srcs)
        if (s.rate != 44100 || s.bps != 16 || s.ch != 2) need_conv = true;
    if (need_conv) {
        std::string desc;
        for (auto &s : tl.srcs) {
            std::string f = fmtName(s.rate, s.bps, s.ch);
            if (desc.find(f) == std::string::npos) desc += (desc.empty() ? "" : ", ") + f;
        }
        if (!allow_conv)
            fail(FISO_E_UNSUPPORTED, "[needs_conversion] the sources are " + desc +
                                         "; an audio CD requires 44.1 kHz / 16 bit / stereo. "
                                         "Allow the conversion explicitly to create a burnable image.");
        uint64_t all = 0, done = 0;
        for (auto &s : tl.srcs) all += s.len;
        std::map<int, double> ratio;
        std::vector<Src> conv = tl.srcs;
        for (size_t i = 0; i < tl.srcs.size(); i++) {
            auto &s = tl.srcs[i];
            ratio[(int)i] = 44100.0 / s.rate;
            if (s.rate == 44100 && s.bps == 16 && s.ch == 2) {
                done += s.len;
                continue;
            }
            std::string tmp = tempPathFor(out) + ".src" + std::to_string(i) + ".pcm";
            temps.push_back(tmp);
            uint64_t frames = convertToCd(s, tmp, prog, done, all);
            conv[i] = Src{tmp, SK_RAW_LE, frames, false, 44100, 2, 16};
        }
        // remap pieces and index positions to the converted time base
        std::vector<Piece> np;
        std::vector<std::pair<uint64_t, uint64_t>> map;  // old piece start -> new piece start
        uint64_t oldpos = 0, newpos = 0;
        for (auto &p : tl.pieces) {
            double r = p.src >= 0 ? ratio[p.src] : 44100.0 / tl.rate;
            uint64_t a = (uint64_t)std::llround(p.src_start * r), b = (uint64_t)std::llround((p.src_start + p.length) * r);
            if (p.src < 0) {
                a = 0;
                b = (uint64_t)std::llround(p.length * r);
            }
            np.push_back({p.src, p.src >= 0 ? a : 0, b - a});
            map.push_back({oldpos, newpos});
            oldpos += p.length;
            newpos += b - a;
        }
        auto remap = [&](uint64_t pos) -> uint64_t {
            for (size_t i = tl.pieces.size(); i-- > 0;) {
                if (pos >= map[i].first) {
                    auto &p = tl.pieces[i];
                    double r = p.src >= 0 ? ratio[p.src] : 44100.0 / tl.rate;
                    uint64_t base_src = p.src >= 0 ? p.src_start : 0;
                    uint64_t local =
                        (uint64_t)std::llround((base_src + (pos - map[i].first)) * r) - (uint64_t)std::llround(base_src * r);
                    return std::min(map[i].second + local, newpos);
                }
            }
            return 0;
        };
        for (auto &t : tl.tracks)
            for (auto &x : t.idx) x.second = remap(x.second);
        for (auto &p : np)
            if (p.src >= 0) p.length = std::min(p.length, conv[p.src].len - std::min(p.src_start, conv[p.src].len));
        tl.pieces = np;
        tl.srcs = conv;
        rep["converted_from"] = desc;
        tl.warnings.push_back(
            "[converted] the audio was converted from " + desc +
            " to 44.1 kHz / 16 bit / stereo for CD "
            "(resampled where needed, dithered): the result is a CD version, not a bit-exact copy of the source");
        for (auto &t : tl.tracks) t.passthrough_src = -1;
    }
    tl.rate = 44100;
    tl.ch = 2;
    tl.bps = 16;

    // remember each track's original range to know whether it changed
    auto trackRanges = [&]() {
        std::vector<std::pair<uint64_t, uint64_t>> r;
        uint64_t total = tl.total();
        for (size_t i = 0; i < tl.tracks.size(); i++) {
            uint64_t a = 0, b = total;
            for (auto &x : tl.tracks[i].idx)
                if (x.first == 1) a = x.second;
            if (i + 1 < tl.tracks.size())
                for (auto &x : tl.tracks[i + 1].idx)
                    if (x.first == 1) b = x.second;
            r.push_back({a, b});
        }
        return r;
    };
    auto before = trackRanges();

    // 2) sector alignment: every index moves to the nearest sector boundary; no audio is
    //    added or removed between tracks, so gap-less programmes stay gap-less.
    uint64_t total = tl.total(), maxshift = 0;
    int moved = 0;
    int64_t prev = -1;
    for (auto &t : tl.tracks) {
        std::sort(t.idx.begin(), t.idx.end(),
                  [](auto &a, auto &b) { return a.second < b.second || (a.second == b.second && a.first < b.first); });
        for (auto &x : t.idx) {
            uint64_t r = (x.second + kSectorSamples / 2) / kSectorSamples * kSectorSamples;
            if (r > total) r -= kSectorSamples;
            if ((int64_t)r <= prev) r = (uint64_t)prev + kSectorSamples;  // keep strictly increasing
            uint64_t d = r > x.second ? r - x.second : x.second - r;
            if (d) moved++;
            maxshift = std::max(maxshift, d);
            x.second = r;
            prev = (int64_t)r;
        }
        std::sort(t.idx.begin(), t.idx.end());
    }
    if (moved) {
        rep["moved_boundaries"] = moved;
        rep["max_shift_ms"] = std::round(maxshift * 10000.0 / 44100.0) / 10.0;
    }
    uint64_t tail = (kSectorSamples - total % kSectorSamples) % kSectorSamples;
    // a boundary rounded beyond the end needs the padding too
    for (auto &t : tl.tracks)
        for (auto &x : t.idx)
            if (x.second >= total + tail) tail += kSectorSamples;
    if (tail) {
        tl.pieces.push_back({-1, 0, tail});
        tl.synth_silence = true;
        rep["end_padding_samples"] = tail;
    }

    auto after = trackRanges();
    for (size_t i = 0; i < tl.tracks.size(); i++) {
        auto &t = tl.tracks[i];
        if (t.passthrough_src < 0) continue;
        const Src &s = tl.srcs[t.passthrough_src];
        if (after[i] != before[i] || after[i].second - after[i].first != s.len) t.passthrough_src = -1;
    }
    if (rep.contains("moved_boundaries"))
        tl.warnings.push_back("[aligned] " + std::to_string(rep["moved_boundaries"].get<int>()) +
                              " track boundary(ies) moved to the nearest CD sector (at most " + rep["max_shift_ms"].dump() +
                              " ms); no audio was added or removed between tracks");
    if (tail) tl.warnings.push_back("[end_padding] " + std::to_string(tail) + " samples of silence complete the last CD sector");
    tl.mastering = rep;
}

}  // namespace

json createImage(const json &plan, const Progress &prog) {
    std::string out = plan.at("output").get<std::string>();
    bool overwrite = plan.value("overwrite", false);
    int level = plan.value("compression", 8);
    if (level < 0 || level > 8) fail(FISO_E_ARG, "compression level must be 0-8");
    bool allow_pass = plan.value("flac_passthrough", true);
    const json &src = plan.at("source");
    std::string kind = src.at("kind");

    Timeline tl;
    bool for_cd = plan.value("cd_compatible", false);
    // FISO images are archives: creation from files, CUE sheets or discs never alters the audio.
    // A burnable CD version is a separate, explicit derivation of an existing image.
    if (for_cd && kind != "fiso")
        fail(FISO_E_ARG,
             "[cd_version_from_fiso] cd_compatible applies only to source kind 'fiso' (create the archive image first, then "
             "derive its CD version)");
    if (kind == "files")
        buildFromFiles(tl, src.at("files"));
    else if (kind == "cue")
        buildFromCue(tl, src.at("path").get<std::string>());
    else if (kind == "raw")
        buildFromRaw(tl, src);
    else if (kind == "fiso")
        buildFromFiso(tl, src.at("path").get<std::string>());
    else
        fail(FISO_E_ARG, "unknown source kind '" + kind + "'");
    if (!overwrite && pathExists(out)) fail(FISO_E_EXISTS, "'" + out + "' already exists");

    std::vector<std::string> temps;
    struct Cleanup {
        std::vector<std::string> &t;
        ~Cleanup() {
            for (auto &p : t) removeFile(p);
        }
    } cleanup{temps};
    if (for_cd) masterForCd(tl, plan, out, temps, prog);

    uint64_t total = tl.total();
    if (total == 0) fail(FISO_E_FORMAT, "source contains no audio");
    if (tl.tracks.empty() || tl.tracks.size() > 99) fail(FISO_E_FORMAT, "1 to 99 tracks required");

    // ---- layout
    Layout L;
    L.rate = tl.rate;
    L.channels = (uint8_t)tl.ch;
    L.bps = (uint8_t)tl.bps;
    L.total = total;
    L.start_lba = tl.start_lba;
    L.mcn = tl.mcn;
    L.first_track = (uint8_t)tl.tracks.front().number;
    L.last_track = (uint8_t)tl.tracks.back().number;
    for (size_t i = 0; i < tl.tracks.size(); i++) {
        auto &pt = tl.tracks[i];
        std::sort(pt.idx.begin(), pt.idx.end());
        TrackRec t;
        t.number = (uint8_t)pt.number;
        t.flags = pt.flags;
        t.isrc = pt.isrc;
        t.first_index = (uint16_t)L.indexes.size();
        t.index_count = (uint16_t)pt.idx.size();
        bool has01 = false;
        for (auto &[n, p] : pt.idx) {
            if (n == 1) {
                t.start = p;
                has01 = true;
            }
            L.indexes.push_back({(uint8_t)pt.number, (uint8_t)n, p});
        }
        if (!has01) fail(FISO_E_FORMAT, "track " + std::to_string(pt.number) + " has no INDEX 01");
        L.tracks.push_back(t);
    }
    for (size_t i = 0; i < L.tracks.size(); i++) {
        uint64_t end = i + 1 < L.tracks.size() ? L.tracks[i + 1].start : total;
        if (end <= L.tracks[i].start)
            fail(FISO_E_FORMAT, "track " + std::to_string(L.tracks[i].number) + " has zero or negative length");
        L.tracks[i].length = end - L.tracks[i].start;
    }
    if (L.tracks[0].start > 0) {
        L.flags |= LF_HTOA;
        SegmentRec s;
        s.kind = 0;
        s.track = 0;
        s.start = 0;
        s.length = L.tracks[0].start;
        L.segments.push_back(s);
    }
    for (auto &t : L.tracks) {
        SegmentRec s;
        s.kind = 1;
        s.track = t.number;
        s.start = t.start;
        s.length = t.length;
        L.segments.push_back(s);
    }
    if (tl.synth_silence) L.flags |= LF_SYNTH_SILENCE;
    if (tl.physical) L.flags |= LF_PHYSICAL;
    if (computeCddaFlag(L)) L.flags |= LF_CDDA;
    std::vector<std::string> warnings = tl.warnings;
    if (!tl.toc.empty()) {
        L.toc = tl.toc;
        L.flags |= LF_HAS_TOC;
    } else if (L.flags & LF_CDDA) {
        for (auto &t : L.tracks) {
            TocRec r;
            r.track = t.number;
            r.control = (t.flags & 1 ? 1 : 0) | (t.flags & 2 ? 2 : 0) | (t.flags & 4 ? 8 : 0);
            r.lba = L.start_lba + (int32_t)(t.start / kSectorSamples);
            L.toc.push_back(r);
        }
        TocRec lo;
        lo.track = 0xAA;
        lo.lba = L.start_lba + (int32_t)(total / kSectorSamples);
        L.toc.push_back(lo);
        L.flags |= LF_HAS_TOC;
    }

    // ---- metadata and pictures are validated before any audio is encoded
    json meta = mergeMeta(tl.hint_meta, plan.value("meta", json::object()));
    meta = normalizeMeta(meta, L, true);
    if (tl.kind == "fiso") {
        json old = Image::open(src.at("path").get<std::string>())->meta;
        meta["provenance"] = old.value("provenance", json::object());
        meta["provenance"]["creator"] = std::string("libfiso ") + FISO_LIB_VERSION;
        meta["provenance"]["created"] = nowIso8601();
        meta["provenance"]["derived_from"] = baseName(src.at("path").get<std::string>());
        if (meta["provenance"].contains("rip")) meta["provenance"]["rip"]["verified"] = false;  // new audio, new verification
    } else {
        meta["provenance"]["source"] = tl.kind == "files" ? "files" : tl.kind == "cue" ? "cue" : "cd";
    }
    if (!tl.mastering.is_null()) meta["provenance"]["cd_mastering"] = tl.mastering;
    if (plan.contains("rip")) meta["provenance"]["rip"] = plan["rip"];
    if (tl.synth_silence) meta["provenance"]["synthesized_silence"] = true;
    validateMeta(meta, L);
    std::vector<Picture> pictures = tl.pictures;
    if (plan.contains("pictures") && !plan["pictures"].empty()) pictures.clear();
    if (plan.contains("pictures"))
        for (auto &p : plan["pictures"]) {
            pictures.push_back(pictureFromFile(p.at("path").get<std::string>(), p.value("type", 3u)));
            pictures.back().desc = p.value("description", std::string());
        }

    // ---- write
    ContainerWriter w(out, overwrite);
    Sha256 tsha;
    uint64_t done = 0;
    bool verify = plan.value("verify", true);
    uint64_t all = total * (verify ? 2 : 1);
    json passed = json::array();
    for (auto &s : L.segments) {
        const PTrack *pt = nullptr;
        for (auto &t : tl.tracks)
            if (s.kind == 1 && t.number == s.track) pt = &t;
        bool used = false;
        if (allow_pass && pt && pt->passthrough_src >= 0) {
            size_t idx_before = w.entries().size();
            used = passthroughSegment(w, tl.srcs[pt->passthrough_src], tl, s.sha, tsha, prog, done, all);
            if (used) {
                s.chunk_index = (uint32_t)idx_before;
                passed.push_back(s.track);
            }
        }
        if (!used) {
            s.chunk_index = (uint32_t)w.entries().size();
            encodeSegment(w, tl, s.start, s.length, level, s.sha, tsha, prog, done, all);
        }
    }
    L.timeline_sha = tsha.finish();
    auto layt = serializeLayout(L);
    parseLayout(layt.data(), layt.size());  // self-check against the specification rules
    w.addChunk("LAYT", kChunkCritical, layt);

    std::string mdump = meta.dump(2);
    w.addChunk("META", kChunkCritical, std::vector<uint8_t>(mdump.begin(), mdump.end()));
    for (auto &pic : pictures) w.addChunk("PICT", 0, serializePicture(pic));
    if (plan.contains("cdtext_packs") && plan["cdtext_packs"].is_string()) {
        auto packs = readWholeFile(plan["cdtext_packs"].get<std::string>(), 1 << 20);
        if (!packs.empty()) w.addChunk("CDTX", 0, packs);
    }
    if (plan.contains("log") && plan["log"].is_string()) {
        std::string lg = plan["log"];
        w.addChunk("RLOG", 0, std::vector<uint8_t>(lg.begin(), lg.end()));
    }
    w.finish();

    uint64_t pcm = L.pcmBytes();
    json rep = {
        {"output", out},
        {"tracks", L.tracks.size()},
        {"total_samples", total},
        {"duration_seconds", (double)total / L.rate},
        {"pcm_bytes", pcm},
        {"fiso_bytes", w.finalSize()},
        {"ratio", pcm ? (double)w.finalSize() / (double)pcm : 0.0},
        {"cdda", L.cdda()},
        {"htoa", bool(L.flags & LF_HTOA)},
        {"flac_passthrough_tracks", passed},
        {"warnings", warnings},
    };
    if (verify) {
        Progress sub;
        struct Ctx {
            const Progress *p;
            uint64_t base, all;
        } ctx{&prog, total, all};
        sub.user = &ctx;
        sub.cb = [](void *u, const char *, uint64_t d, uint64_t t) -> int {
            // verification fills the second half of the bar, whatever its own units are
            auto *c = (Ctx *)u;
            double f = t ? std::min(1.0, (double)d / (double)t) : 0.0;
            (*c->p)("verify", c->base + (uint64_t)(f * (double)(c->all - c->base)), c->all);
            return 0;
        };
        rep["verify"] = verifyImage(out, true, sub);
    }
    return rep;
}

}  // namespace fiso
