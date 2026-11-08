// FISO conformance and regression tests. Uses only synthetic audio (sines, noise,
// silence, full-scale values) so the generated files may be redistributed freely.
// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace fiso;
namespace fs = std::filesystem;
static constexpr double kPi = 3.14159265358979323846;

int g_fail = 0, g_pass = 0;
std::string g_work;
void testCd();

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (c)                                                                     \
            g_pass++;                                                              \
        else {                                                                     \
            g_fail++;                                                              \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #c "\n"; \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        auto _a = (a);                                                                                    \
        auto _b = (b);                                                                                    \
        if (_a == _b)                                                                                     \
            g_pass++;                                                                                     \
        else {                                                                                            \
            g_fail++;                                                                                     \
            std::cerr << "  FAIL " << __LINE__ << "  " #a " == " #b "  (" << _a << " vs " << _b << ")\n"; \
        }                                                                                                 \
    } while (0)

static std::string W(const std::string &n) {
    return (fs::path(g_work) / n).string();
}

// ---------------------------------------------------------------- signal generation
struct Gen {
    double phase = 0;
    uint32_t lcg = 12345;
    // continuous sine: the phase carries across calls, so cutting the result
    // into tracks produces a gap-less transition that must survive the round trip
    std::vector<int32_t> sine(size_t frames, uint32_t ch, double freq, double rate, double amp) {
        std::vector<int32_t> v(frames * ch);
        for (size_t i = 0; i < frames; i++) {
            double s = std::sin(phase) * amp;
            phase += 2 * kPi * freq / rate;
            for (uint32_t c = 0; c < ch; c++) v[i * ch + c] = (int32_t)std::lround(c ? -s : s);
        }
        return v;
    }
    std::vector<int32_t> noise(size_t frames, uint32_t ch, int32_t lo, int32_t hi) {
        std::vector<int32_t> v(frames * ch);
        for (auto &x : v) {
            lcg = lcg * 1664525u + 1013904223u;
            x = lo + (int32_t)((uint64_t)lcg * (uint64_t)(hi - lo + 1) >> 32);
        }
        return v;
    }
    std::vector<int32_t> extremes(size_t frames, uint32_t ch, uint32_t bps) {
        int32_t mx = (1 << (bps - 1)) - 1, mn = -(1 << (bps - 1));
        std::vector<int32_t> v(frames * ch);
        for (size_t i = 0; i < v.size(); i++) v[i] = (i / 3) % 4 == 0 ? mx : (i / 3) % 4 == 1 ? mn : (i % 2 ? mx - 1 : mn + 1);
        return v;
    }
};

static void append(std::vector<int32_t> &a, const std::vector<int32_t> &b) {
    a.insert(a.end(), b.begin(), b.end());
}

static void writeWav(const std::string &path, uint32_t rate, uint32_t ch, uint32_t bps, const std::vector<int32_t> &s) {
    uint32_t bytes = (bps + 7) / 8;
    std::vector<uint8_t> h;
    auto str = [&](const char *t) { h.insert(h.end(), t, t + 4); };
    uint32_t data = (uint32_t)(s.size() * bytes);
    str("RIFF");
    put32(h, 36 + data);
    str("WAVE");
    str("fmt ");
    put32(h, 16);
    put16(h, 1);
    put16(h, (uint16_t)ch);
    put32(h, rate);
    put32(h, rate * ch * bytes);
    put16(h, (uint16_t)(ch * bytes));
    put16(h, (uint16_t)bps);
    str("data");
    put32(h, data);
    canonicalBytes(s.data(), s.size(), bps, h);
    File f = File::open(path, "wb");
    f.write(h.data(), h.size());
}

static void writeBin(const std::string &path, const std::vector<int32_t> &s) {
    std::vector<uint8_t> b;
    canonicalBytes(s.data(), s.size(), 16, b);
    File f = File::open(path, "wb");
    f.write(b.data(), b.size());
}

static void writeText(const std::string &path, const std::string &t) {
    File f = File::open(path, "wb");
    f.write(t.data(), t.size());
}

// Takes ownership of a JSON result returned by the C API. A failed call leaves the
// result null: return an empty object so the following checks fail instead of crashing.
static json takeResult(char *&res) {
    json j = res ? json::parse(res) : json::object();
    fiso_free(res);
    res = nullptr;
    return j;
}

static std::vector<int32_t> readAll(const std::string &img_path) {
    fiso_image *img = nullptr;
    if (fiso_open(img_path.c_str(), &img) != FISO_OK) {
        std::cerr << "  open failed: " << fiso_last_error() << "\n";
        return {};
    }
    fiso_format f;
    fiso_get_format(img, &f);
    std::vector<int32_t> v(f.total_samples * f.channels);
    int64_t n = fiso_read_pcm_s32(img, 0, f.total_samples, v.data());
    fiso_close(img);
    if (n != (int64_t)f.total_samples) v.clear();
    return v;
}

static json create(const json &plan, int *status = nullptr) {
    char *r = nullptr;
    int st = fiso_create(plan.dump().c_str(), nullptr, nullptr, &r);
    if (status) *status = st;
    if (st != FISO_OK) {
        if (!status) std::cerr << "  create failed: " << fiso_last_error() << "\n";
        return json();
    }
    json j = json::parse(r);
    fiso_free(r);
    return j;
}

static json verify(const std::string &p) {
    char *r = nullptr;
    if (fiso_verify(p.c_str(), 1, nullptr, nullptr, &r) != FISO_OK) return json();
    json j = json::parse(r);
    fiso_free(r);
    return j;
}

static json info(const std::string &p) {
    fiso_image *img = nullptr;
    if (fiso_open(p.c_str(), &img) != FISO_OK) return json();
    char *s = fiso_info_json(img);
    fiso_close(img);
    json j = json::parse(s);
    fiso_free(s);
    return j;
}

static bool verifiedOk(const json &v) {
    return v.is_object() && v["valid_file"]["ok"] == true && v["intact_audio"]["ok"] == true;
}

static std::vector<uint8_t> readFileBytes(const std::string &p) {
    return readWholeFile(p, 1ull << 32);
}

// audio frames of a FLAC stream (everything after the metadata blocks)
static std::vector<uint8_t> framesOf(const std::vector<uint8_t> &f) {
    size_t p = 4;
    bool last = false;
    while (!last && p + 4 <= f.size()) {
        last = f[p] & 0x80;
        p += 4 + ((size_t)f[p + 1] << 16 | (size_t)f[p + 2] << 8 | f[p + 3]);
    }
    return std::vector<uint8_t>(f.begin() + std::min(p, f.size()), f.end());
}

// tiny valid 1x1 PNG (synthetic)
static const uint8_t kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
                               0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
                               0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08,
                               0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D,
                               0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

// ================================================================ tests
static void testLosslessFiles() {
    std::cerr << "[lossless round trip from WAV files]\n";
    Gen g;
    const uint32_t S = 588;
    std::vector<int32_t> t1 = g.sine(S * 300, 2, 441, 44100, 20000);  // gap-less into t2
    std::vector<int32_t> t2 = g.sine(S * 200, 2, 441, 44100, 20000);
    append(t2, g.extremes(S * 50, 2, 16));    // full-scale values
    std::vector<int32_t> t3(S * 120 * 2, 0);  // digital silence
    t3[1000] = 1;
    t3[1001] = -1;                                  // ...almost
    append(t3, g.noise(S * 80, 2, -32768, 32767));  // incompressible noise
    writeWav(W("t1.wav"), 44100, 2, 16, t1);
    writeWav(W("t2.wav"), 44100, 2, 16, t2);
    writeWav(W("t3.wav"), 44100, 2, 16, t3);
    std::vector<int32_t> all = t1;
    append(all, t2);
    append(all, t3);

    json r = create({{"output", W("files.fiso")},
                     {"source", {{"kind", "files"}, {"files", {W("t1.wav"), W("t2.wav"), W("t3.wav")}}}},
                     {"meta",
                      {{"album", {{"title", "Synthetic Album"}, {"artist", "libfiso tests"}}},
                       {"tracks", {{{"number", 1}, {"title", "Sine A"}}, {{"number", 2}, {"title", "Sine B / extremes"}}}}}}});
    CHECK(r.is_object());
    if (!r.is_object()) return;
    CHECK(r["cdda"] == true);
    CHECK(verifiedOk(r["verify"]));
    CHECK_EQ(r["pcm_bytes"].get<uint64_t>(), (uint64_t)all.size() * 2);
    auto back = readAll(W("files.fiso"));
    CHECK(back == all);

    // random access across a segment boundary
    fiso_image *img;
    CHECK_EQ(fiso_open(W("files.fiso").c_str(), &img), FISO_OK);
    std::vector<int32_t> part(1000 * 2);
    uint64_t at = S * 300 - 400;
    CHECK_EQ(fiso_read_pcm_s32(img, at, 1000, part.data()), 1000);
    CHECK(std::equal(part.begin(), part.end(), all.begin() + at * 2));
    CHECK_EQ(fiso_read_pcm_s32(img, 17, 3, part.data()), 3);  // backwards seek
    CHECK(std::equal(part.begin(), part.begin() + 6, all.begin() + 34));
    fiso_close(img);

    // WAV export is bit-identical to the source WAV files
    char *res = nullptr;
    json o = {{"format", "wav"}, {"out_dir", W("wav_out")}};
    CHECK_EQ(fiso_extract(W("files.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    json ex = takeResult(res);
    CHECK_EQ(ex["files"].size(), (size_t)3);
    if (ex["files"].size() == 3) {
        CHECK(readFileBytes(ex["files"][0]) == readFileBytes(W("t1.wav")));
        CHECK(readFileBytes(ex["files"][2]) == readFileBytes(W("t3.wav")));
        CHECK(fs::path(ex["files"][0].get<std::string>()).filename() == "01 - Sine A.wav");
        CHECK(fs::path(ex["files"][2].get<std::string>()).filename() == "03 - Track 3.wav");
    }
    // never overwrite silently
    CHECK_EQ(fiso_extract(W("files.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_E_EXISTS);
}

static void testCueLayout() {
    std::cerr << "[CUE: HTOA, INDEX 00 pre-gaps, INDEX 02, ISRC, MCN, flags, BIN/CUE rebuild]\n";
    Gen g;
    const uint32_t S = 588;
    std::vector<int32_t> pcm = g.sine(S * 100, 2, 300, 44100, 8000);  // hidden track one audio
    append(pcm, g.sine(S * 900, 2, 500, 44100, 12000));
    append(pcm, std::vector<int32_t>(S * 150 * 2, 0));  // 2 s silent pre-gap of track 2
    append(pcm, g.noise(S * 700, 2, -3000, 3000));
    append(pcm, g.sine(S * 400, 2, 700, 44100, 30000));
    writeBin(W("disc.bin"), pcm);
    writeText(W("disc.cue"),
              "REM GENRE \"Test\"\nREM DATE 2026\nCATALOG 0123456789012\nPERFORMER \"Cue Artist\"\nTITLE \"Cue Album\"\n"
              "FILE \"disc.bin\" BINARY\n"
              "  TRACK 01 AUDIO\n    TITLE \"One\"\n    FLAGS PRE DCP\n    ISRC FRXXX2600001\n    INDEX 00 00:00:00\n    INDEX "
              "01 00:01:25\n"
              "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 00 00:13:25\n    INDEX 01 00:15:25\n    INDEX 02 00:20:00\n"
              "  TRACK 03 AUDIO\n    TITLE \"Three\"\n    INDEX 01 00:24:50\n");
    json r = create({{"output", W("cue.fiso")},
                     {"source", {{"kind", "cue"}, {"path", W("disc.cue")}}},
                     {"meta", {{"disc", {{"number", 1}, {"total", 1}}}}}});
    CHECK(r.is_object());
    if (!r.is_object()) return;
    CHECK(r["htoa"] == true);
    CHECK(r["cdda"] == true);
    CHECK(verifiedOk(r["verify"]));
    json i = info(W("cue.fiso"));
    CHECK_EQ(i["mcn"].get<std::string>(), std::string("0123456789012"));
    CHECK_EQ(i["htoa_samples"].get<uint64_t>(), (uint64_t)S * 100);
    CHECK_EQ(i["meta"]["album"]["title"].get<std::string>(), std::string("Cue Album"));
    CHECK_EQ(i["meta"]["tracks"][1]["title"].get<std::string>(), std::string("Two"));
    auto &t = i["tracks"];
    CHECK_EQ(t[0]["isrc"].get<std::string>(), std::string("FRXXX2600001"));
    CHECK(t[0]["pre_emphasis"] == true && t[0]["copy_permitted"] == true);
    CHECK_EQ(t[1]["pregap_samples"].get<uint64_t>(), (uint64_t)S * 150);
    CHECK_EQ(t[1]["indexes"].size(), (size_t)3);
    CHECK_EQ(t[1]["start"].get<uint64_t>(), (uint64_t)S * 1150);
    CHECK_EQ(t[2]["start"].get<uint64_t>(), (uint64_t)S * 1850);
    CHECK(readAll(W("cue.fiso")) == pcm);

    // BIN/CUE rebuild: identical BIN, CUE that re-creates an identical timeline
    char *res = nullptr;
    json o = {{"format", "bin-cue"}, {"out_dir", W("bincue")}};
    CHECK_EQ(fiso_extract(W("cue.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    json ex = takeResult(res);
    CHECK(readFileBytes(ex["files"][0]) == readFileBytes(W("disc.bin")));
    json r2 = create({{"output", W("cue2.fiso")}, {"source", {{"kind", "cue"}, {"path", ex["files"][1]}}}});
    CHECK(r2.is_object());
    json i2 = info(W("cue2.fiso"));
    CHECK(i2["tracks"] == i["tracks"]);
    CHECK_EQ(i2["mcn"], i["mcn"]);

    // HTOA export
    o = {{"format", "flac"}, {"out_dir", W("htoa")}, {"include_htoa", true}};
    CHECK_EQ(fiso_extract(W("cue.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    ex = takeResult(res);
    CHECK_EQ(ex["files"].size(), (size_t)4);
}

static void testPregapSilence() {
    std::cerr << "[CUE PREGAP synthesised silence and multi-FILE sheets]\n";
    Gen g;
    auto a = g.sine(588 * 200, 2, 440, 44100, 10000), b = g.sine(588 * 100, 2, 440, 44100, 10000);
    writeWav(W("pa.wav"), 44100, 2, 16, a);
    writeWav(W("pb.wav"), 44100, 2, 16, b);
    writeText(W("multi.cue"),
              "TITLE \"Multi\"\nFILE \"pa.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
              "FILE \"pb.wav\" WAVE\n  TRACK 02 AUDIO\n    PREGAP 00:01:00\n    INDEX 01 00:00:00\n");
    json r = create({{"output", W("pregap.fiso")}, {"source", {{"kind", "cue"}, {"path", W("multi.cue")}}}});
    CHECK(r.is_object());
    if (!r.is_object()) return;
    json i = info(W("pregap.fiso"));
    CHECK(i["synthesized_silence"] == true);
    CHECK_EQ(i["tracks"][1]["pregap_samples"].get<uint64_t>(), (uint64_t)44100);
    auto all = a;
    append(all, std::vector<int32_t>(44100 * 2, 0));
    append(all, b);
    CHECK(readAll(W("pregap.fiso")) == all);
}

static void testMultiDisc() {
    std::cerr << "[multi-disc albums]\n";
    Gen g;
    std::string gid = uuid4();
    std::vector<std::string> paths;
    for (int d = 1; d <= 3; d++) {
        std::string wav = W("d" + std::to_string(d) + ".wav");
        writeWav(wav, 44100, 2, 16, g.sine(588 * (50 + d), 2, 200.0 * d, 44100, 9000));
        std::string out = W("Album - Disc " + std::to_string(d) + ".fiso");
        json r = create({{"output", out},
                         {"source", {{"kind", "files"}, {"files", {wav}}}},
                         {"meta",
                          {{"album", {{"title", "Box Set"}, {"artist", "Tests"}, {"group_id", gid}}},
                           {"disc", {{"number", d}, {"total", 3}, {"subtitle", "Part " + std::to_string(d)}}}}}});
        CHECK(r.is_object());
        if (d != 2)
            paths.push_back(out);
        else
            paths.insert(paths.begin(), out);
    }
    // invalid: disc number greater than total
    int st = 0;
    create({{"output", W("bad.fiso")},
            {"source", {{"kind", "files"}, {"files", {W("d1.wav")}}}},
            {"meta", {{"disc", {{"number", 4}, {"total", 3}}}}}},
           &st);
    CHECK_EQ(st, FISO_E_FORMAT);
    CHECK(!pathExists(W("bad.fiso")));

    char *res = nullptr;
    CHECK_EQ(fiso_group(json(paths).dump().c_str(), &res), FISO_OK);
    json gr = takeResult(res);
    CHECK_EQ(gr["groups"].size(), (size_t)1);
    auto &grp = gr["groups"][0];
    CHECK_EQ(grp["method"].get<std::string>(), std::string("group_id"));
    CHECK_EQ(grp["discs"].size(), (size_t)3);
    CHECK_EQ(grp["discs"][1]["number"].get<int>(), 2);
    CHECK(grp["missing_discs"].empty());
    // a missing disc is reported
    CHECK_EQ(fiso_group(json({paths[0], paths[2]}).dump().c_str(), &res), FISO_OK);
    gr = takeResult(res);
    CHECK_EQ(gr["groups"][0]["missing_discs"].size(), (size_t)1);
    // each disc is independent and its numbering survives FLAC export
    json o = {{"format", "flac"}, {"out_dir", W("disc2")}};
    CHECK_EQ(fiso_extract(W("Album - Disc 2.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    json ex = takeResult(res);
    std::string name = fs::path(ex["files"][0].get<std::string>()).filename().string();
    CHECK_EQ(name, std::string("Disc 2 - 01 - Track 1.flac"));
    auto bytes = readFileBytes(ex["files"][0]);
    std::string s(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), 4096));
    CHECK(s.find("DISCNUMBER=2") != std::string::npos);
    CHECK(s.find("DISCTOTAL=3") != std::string::npos);
    CHECK(s.find("ALBUM_GROUP_ID=" + gid) != std::string::npos);
}

static void testFlacPassthroughAndEdit() {
    std::cerr << "[FLAC pass-through, metadata edit without touching audio]\n";
    // FLAC sources: export the tracks of files.fiso
    char *res = nullptr;
    json o = {{"format", "flac"}, {"out_dir", W("flac_src")}};
    CHECK_EQ(fiso_extract(W("files.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    json ex = takeResult(res);
    json r = create({{"output", W("pass.fiso")}, {"source", {{"kind", "files"}, {"files", ex["files"]}}}});
    CHECK(r.is_object());
    if (!r.is_object()) return;
    CHECK_EQ(r["flac_passthrough_tracks"].size(), (size_t)3);
    CHECK(verifiedOk(r["verify"]));
    CHECK(readAll(W("pass.fiso")) == readAll(W("files.fiso")));
    // audio frames of the stored segments are byte-identical
    auto a = Image::open(W("files.fiso")), b = Image::open(W("pass.fiso"));
    for (size_t k = 0; k < a->layout.segments.size(); k++) {
        auto x = a->readChunk(a->layout.segments[k].chunk_index, 1ull << 31);
        auto y = b->readChunk(b->layout.segments[k].chunk_index, 1ull << 31);
        CHECK(framesOf(x) == framesOf(y));
    }
    std::vector<Hash> before;
    for (auto &s : b->layout.segments) before.push_back(b->chunks[s.chunk_index].sha);
    a.reset();
    b.reset();

    writeText(W("cover.png"), std::string((const char *)kPng, sizeof kPng));
    json m = info(W("pass.fiso"))["meta"];
    m["album"]["title"] = "Édition « spéciale » ★";
    m["tracks"][0]["title"] = "Première piste";
    json edit = {{"meta", m}, {"pictures", {{{"type", 3}, {"path", W("cover.png")}}}}};
    CHECK_EQ(fiso_edit(W("pass.fiso").c_str(), edit.dump().c_str(), nullptr, nullptr, nullptr), FISO_OK);
    auto c = Image::open(W("pass.fiso"));
    for (size_t k = 0; k < c->layout.segments.size(); k++) CHECK(c->chunks[c->layout.segments[k].chunk_index].sha == before[k]);
    CHECK_EQ(c->meta["album"]["title"].get<std::string>(), std::string("Édition « spéciale » ★"));
    CHECK_EQ(c->pict_chunks.size(), (size_t)1);
    if (!c->pict_chunks.empty()) {
        Picture p = c->readPicture(c->pict_chunks[0]);
        CHECK_EQ(p.mime, std::string("image/png"));
        CHECK_EQ(p.width, 1u);
    }
    c.reset();
    CHECK(verifiedOk(verify(W("pass.fiso"))));
    // invalid metadata is refused and the original is untouched
    json badm = m;
    badm["tracks"].push_back({{"number", 42}});
    auto orig = readFileBytes(W("pass.fiso"));
    CHECK_EQ(fiso_edit(W("pass.fiso").c_str(), json({{"meta", badm}}).dump().c_str(), nullptr, nullptr, nullptr), FISO_E_FORMAT);
    CHECK(readFileBytes(W("pass.fiso")) == orig);
    o = {{"format", "cover"}, {"out_dir", W("covers")}};
    CHECK_EQ(fiso_extract(W("pass.fiso").c_str(), o.dump().c_str(), nullptr, nullptr, &res), FISO_OK);
    ex = takeResult(res);
    CHECK_EQ(ex["files"].size(), (size_t)1);
}

static void testHiRes() {
    std::cerr << "[non CD-DA source: 48 kHz / 24-bit / 6 channels]\n";
    Gen g;
    auto s = g.noise(48000, 6, -(1 << 23), (1 << 23) - 1);
    append(s, g.extremes(1000, 6, 24));
    writeWav(W("hires.wav"), 48000, 6, 24, s);
    json r = create({{"output", W("hires.fiso")}, {"source", {{"kind", "files"}, {"files", {W("hires.wav")}}}}});
    CHECK(r.is_object());
    if (!r.is_object()) return;
    CHECK(r["cdda"] == false);
    CHECK(r["warnings"].empty());  // a non CD-DA archive is normal, not a warning
    CHECK(verifiedOk(r["verify"]));
    CHECK(readAll(W("hires.fiso")) == s);
    // mixing formats is refused instead of silently converting
    int st = 0;
    create({{"output", W("mix.fiso")}, {"source", {{"kind", "files"}, {"files", {W("hires.wav"), W("t1.wav")}}}}}, &st);
    CHECK_EQ(st, FISO_E_UNSUPPORTED);
}

static void corruptCopy(const std::string &src, const std::string &dst, uint64_t off, uint8_t x) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    File f = File::open(dst, "r+b");
    uint8_t b;
    f.seek(off);
    f.read(&b, 1);
    b ^= x;
    f.seek(off);
    f.write(&b, 1);
}

static void testCorruption() {
    std::cerr << "[corrupted and malformed files]\n";
    std::string src = W("files.fiso");
    fiso_image *img = nullptr;
    // 1) audio byte flipped -> chunk hash and decoded PCM fail, structure still opens
    auto im = Image::open(src);
    auto &c = im->chunks[im->layout.segments[1].chunk_index];
    uint64_t off = c.offset + c.length / 2;
    im.reset();
    corruptCopy(src, W("bad_audio.fiso"), off, 0x5A);
    json v = verify(W("bad_audio.fiso"));
    CHECK(v["valid_file"]["ok"] == false);
    CHECK(v["intact_audio"]["ok"] == false);
    // 2) signature
    corruptCopy(src, W("bad_magic.fiso"), 1, 0x01);
    CHECK_EQ(fiso_open(W("bad_magic.fiso").c_str(), &img), FISO_E_FORMAT);
    // 3) header CRC
    corruptCopy(src, W("bad_hdr.fiso"), 30, 0x01);
    CHECK_EQ(fiso_open(W("bad_hdr.fiso").c_str(), &img), FISO_E_FORMAT);
    // 4) chunk table
    uint64_t size = fs::file_size(src);
    corruptCopy(src, W("bad_table.fiso"), size - 40, 0x01);
    CHECK_EQ(fiso_open(W("bad_table.fiso").c_str(), &img), FISO_E_INTEGRITY);
    // 5) truncated
    fs::copy_file(src, W("trunc.fiso"), fs::copy_options::overwrite_existing);
    fs::resize_file(W("trunc.fiso"), size - 1000);
    CHECK_EQ(fiso_open(W("trunc.fiso").c_str(), &img), FISO_E_FORMAT);
    // 6) future major version
    {
        fs::copy_file(src, W("v2.fiso"), fs::copy_options::overwrite_existing);
        File f = File::open(W("v2.fiso"), "r+b");
        uint8_t h[96];
        f.read(h, 96);
        h[8] = 2;
        uint32_t crc = crc32(h, 92);
        h[92] = crc & 0xff;
        h[93] = (crc >> 8) & 0xff;
        h[94] = (crc >> 16) & 0xff;
        h[95] = crc >> 24;
        f.seek(0);
        f.write(h, 96);
    }
    CHECK_EQ(fiso_open(W("v2.fiso").c_str(), &img), FISO_E_UNSUPPORTED);
    // 7) not a FISO file at all
    CHECK_EQ(fiso_open(W("t1.wav").c_str(), &img), FISO_E_FORMAT);
    // 8) unknown non-critical chunk is ignored, unknown critical chunk refused
    {
        auto s = Image::open(src);
        ContainerWriter w(W("extra.fiso"), true);
        for (size_t i = 0; i < s->chunks.size(); i++) w.copyChunk(*s, i, Progress());
        w.addChunk("XTRA", 0, {1, 2, 3});
        w.finish();
        ContainerWriter w2(W("crit.fiso"), true);
        for (size_t i = 0; i < s->chunks.size(); i++) w2.copyChunk(*s, i, Progress());
        w2.addChunk("XTRA", kChunkCritical, {1, 2, 3});
        w2.finish();
    }
    CHECK_EQ(fiso_open(W("extra.fiso").c_str(), &img), FISO_OK);
    if (img) fiso_close(img);
    CHECK_EQ(fiso_open(W("crit.fiso").c_str(), &img), FISO_E_UNSUPPORTED);
    // 9) existing output is never overwritten silently
    int st = 0;
    create({{"output", src}, {"source", {{"kind", "files"}, {"files", {W("t1.wav")}}}}}, &st);
    CHECK_EQ(st, FISO_E_EXISTS);
    // keep a corrupted sample for the public test-file set
    fs::copy_file(W("bad_audio.fiso"), W("corrupted-audio.fiso"), fs::copy_options::overwrite_existing);
}

static void testCdMastering() {
    std::cerr << "[CD version: an archive image is never altered, a burnable copy is derived on request]\n";
    Gen g;
    // three gap-less tracks whose lengths are not whole CD sectors (as most downloaded/encoded FLACs)
    std::vector<int32_t> a = g.sine(588 * 120 + 173, 2, 330, 44100, 15000), b = g.sine(588 * 90 + 401, 2, 330, 44100, 15000),
                         c = g.sine(588 * 70 + 55, 2, 330, 44100, 15000);
    writeWav(W("u1.wav"), 44100, 2, 16, a);
    writeWav(W("u2.wav"), 44100, 2, 16, b);
    writeWav(W("u3.wav"), 44100, 2, 16, c);
    std::vector<int32_t> all = a;
    append(all, b);
    append(all, c);
    // creation keeps the sources bit for bit: no alignment, no padding, not flagged CD-DA
    json r0 = create({{"output", W("unaligned.fiso")},
                      {"source", {{"kind", "files"}, {"files", {W("u1.wav"), W("u2.wav"), W("u3.wav")}}}},
                      {"meta", {{"album", {{"title", "Sans alignement"}}}}},
                      {"pictures", {{{"type", 3}, {"path", W("cover.png")}}}}});
    CHECK(r0.is_object() && r0["cdda"] == false);
    CHECK(readAll(W("unaligned.fiso")) == all);
    // the CD option is refused at creation time: it only applies to an existing image
    int st = 0;
    create({{"output", W("refused.fiso")}, {"source", {{"kind", "files"}, {"files", {W("u1.wav")}}}}, {"cd_compatible", true}},
           &st);
    CHECK_EQ(st, FISO_E_ARG);
    CHECK(std::string(fiso_last_error()).find("[cd_version_from_fiso]") != std::string::npos);
    CHECK(!fs::exists(W("refused.fiso")));

    // CD version of the archive: boundaries move to the nearest sector, no audio added between tracks
    json r1 = create({{"output", W("cd-from-fiso.fiso")},
                      {"source", {{"kind", "fiso"}, {"path", W("unaligned.fiso")}}},
                      {"cd_compatible", true}});
    CHECK(r1.is_object());
    if (r1.is_object()) {
        CHECK(r1["cdda"] == true);
        CHECK(verifiedOk(r1["verify"]));
        auto back = readAll(W("cd-from-fiso.fiso"));
        CHECK_EQ(back.size() % (588 * 2), (size_t)0);
        CHECK(std::equal(all.begin(), all.end(), back.begin()));                                      // the music is untouched
        CHECK(std::all_of(back.begin() + all.size(), back.end(), [](int32_t v) { return v == 0; }));  // only end padding
        json i = info(W("cd-from-fiso.fiso"));
        uint64_t b1 = a.size() / 2, b2 = (a.size() + b.size()) / 2;
        uint64_t s2 = i["tracks"][1]["start"], s3 = i["tracks"][2]["start"];
        CHECK(s2 % 588 == 0 && s3 % 588 == 0);
        CHECK((s2 > b1 ? s2 - b1 : b1 - s2) <= 294);
        CHECK((s3 > b2 ? s3 - b2 : b2 - s3) <= 294);
        auto im = Image::open(W("cd-from-fiso.fiso"));
        CHECK_EQ(im->meta["album"]["title"].get<std::string>(), std::string("Sans alignement"));
        CHECK_EQ(im->pict_chunks.size(), (size_t)1);
        CHECK(im->meta["provenance"].contains("derived_from"));
        CHECK(im->meta["provenance"]["cd_mastering"]["alignment"] == "shift");
        CHECK_EQ(im->layout.tracks.size(), (size_t)3);
    }
    CHECK(readAll(W("unaligned.fiso")) == all);  // the archive itself is unchanged

    // export for burning software: BIN/CUE in CD format, the image untouched
    char *res = nullptr;
    CHECK_EQ(fiso_extract(W("unaligned.fiso").c_str(), json({{"format", "bin-cue"}, {"out_dir", W("cdbc")}}).dump().c_str(),
                          nullptr, nullptr, &res),
             FISO_OK);
    if (res) {
        json x = takeResult(res);
        CHECK(x["cd_conversion"] == true);
        CHECK_EQ(x["files"].size(), (size_t)2);
        std::string bin = x["files"][0], cue = x["files"][1];
        auto raw = readFileBytes(bin);
        CHECK_EQ(raw.size() % 2352, (size_t)0);
        bool same = raw.size() >= all.size() * 2;
        for (size_t k = 0; same && k < all.size(); k++) same = (int16_t)(raw[2 * k] | (raw[2 * k + 1] << 8)) == all[k];
        CHECK(same);
        auto cueBytes = readFileBytes(cue);
        std::string cueText(cueBytes.begin(), cueBytes.end());
        CHECK(cueText.find("TITLE \"Sans alignement\"") != std::string::npos);
        CHECK(cueText.find("TRACK 03 AUDIO") != std::string::npos);
        // the BIN/CUE is itself a valid CD source: re-importing it gives a CD-DA image
        json rb = create({{"output", W("cdbc.fiso")}, {"source", {{"kind", "cue"}, {"path", cue}}}});
        CHECK(rb.is_object() && rb["cdda"] == true);
        std::vector<std::string> left;
        for (auto &e : fs::directory_iterator(W("cdbc"))) left.push_back(e.path().filename().string());
        CHECK_EQ(left.size(), (size_t)2);  // the intermediate CD version was removed
        // a second export into the same folder is refused before any conversion work
        CHECK_EQ(fiso_extract(W("unaligned.fiso").c_str(), json({{"format", "bin-cue"}, {"out_dir", W("cdbc")}}).dump().c_str(),
                              nullptr, nullptr, &res),
                 FISO_E_EXISTS);
    }

    // high-resolution archive: refused unless the conversion is explicitly accepted
    std::vector<int32_t> hr = g.sine(96000 * 2 + 77, 2, 1000, 96000, 6000000);
    for (auto &v : hr) v = std::clamp(v, -(1 << 23), (1 << 23) - 1);
    writeWav(W("hr96.wav"), 96000, 2, 24, hr);
    json rh = create({{"output", W("hr96.fiso")}, {"source", {{"kind", "files"}, {"files", {W("hr96.wav")}}}}});
    CHECK(rh.is_object() && rh["cdda"] == false);
    CHECK(readAll(W("hr96.fiso")) == hr);  // 24-bit / 96 kHz archived as is
    CHECK_EQ(fiso_extract(W("hr96.fiso").c_str(), json({{"format", "bin-cue"}, {"out_dir", W("cdbc-hr")}}).dump().c_str(),
                          nullptr, nullptr, &res),
             FISO_E_UNSUPPORTED);
    CHECK_EQ(fiso_extract(W("hr96.fiso").c_str(),
                          json({{"format", "bin-cue"}, {"out_dir", W("cdbc-hr")}, {"allow_conversion", true}}).dump().c_str(),
                          nullptr, nullptr, &res),
             FISO_OK);
    if (res) {
        CHECK(json::parse(res)["warnings"].dump().find("[converted]") != std::string::npos);
        fiso_free(res);
        res = nullptr;
    }
    create({{"output", W("cd-hr.fiso")}, {"source", {{"kind", "fiso"}, {"path", W("hr96.fiso")}}}, {"cd_compatible", true}}, &st);
    CHECK_EQ(st, FISO_E_UNSUPPORTED);
    CHECK(std::string(fiso_last_error()).find("[needs_conversion]") != std::string::npos);
    json r4 = create({{"output", W("cd-hr.fiso")},
                      {"source", {{"kind", "fiso"}, {"path", W("hr96.fiso")}}},
                      {"cd_compatible", true},
                      {"allow_conversion", true}});
    CHECK(r4.is_object() && r4["cdda"] == true);
    if (r4.is_object()) {
        auto back = readAll(W("cd-hr.fiso"));
        uint64_t frames = (uint64_t)std::llround((96000.0 * 2 + 77) * 44100 / 96000);
        CHECK(back.size() / 2 >= frames && back.size() / 2 < frames + 588);
        // the 1 kHz tone survives resampling: ~2000 zero crossings per second, level about -2.9 dBFS
        int crossings = 0;
        double peak = 0;
        for (size_t k = 2000; k < 44100 + 2000; k++) {
            if ((back[2 * k] >= 0) != (back[2 * k + 2] >= 0)) crossings++;
            peak = std::max(peak, std::fabs((double)back[2 * k]));
        }
        CHECK(crossings > 1990 && crossings < 2010);
        CHECK(peak > 23000 && peak < 23700);  // 6000000 / 2^23 * 32768 = 23437
        bool warned = false;
        for (auto &w : r4["warnings"]) warned |= w.get<std::string>().find("[converted]") != std::string::npos;
        CHECK(warned);
        auto im = Image::open(W("cd-hr.fiso"));
        CHECK(im->meta["provenance"]["cd_mastering"].contains("converted_from"));
    }
    // mono 16-bit at 44.1 kHz is widened to stereo without any change of sample values
    std::vector<int32_t> mono = g.noise(588 * 10, 1, -32768, 32767);
    writeWav(W("mono.wav"), 44100, 1, 16, mono);
    CHECK(create({{"output", W("mono.fiso")}, {"source", {{"kind", "files"}, {"files", {W("mono.wav")}}}}}).is_object());
    json r5 = create({{"output", W("cd-mono.fiso")},
                      {"source", {{"kind", "fiso"}, {"path", W("mono.fiso")}}},
                      {"cd_compatible", true},
                      {"allow_conversion", true}});
    CHECK(r5.is_object() && r5["cdda"] == true);
    if (r5.is_object()) {
        auto back = readAll(W("cd-mono.fiso"));
        bool same = true;
        for (size_t k = 0; k < mono.size(); k++) same &= back[2 * k] == mono[k] && back[2 * k + 1] == mono[k];
        CHECK(same);
    }
}

static void testProgress() {
    std::cerr << "[progress: encoding 0-50 %, verification 50-100 %, never above 100 %]\n";
    struct P {
        double last = 0, max = 0;
        bool monotonic = true;
        double encMax = 0, verMin = 2;
    };
    P p;
    json plan = {{"output", W("progress.fiso")},
                 {"source", {{"kind", "files"}, {"files", {W("t1.wav"), W("t2.wav"), W("t3.wav")}}}}};
    char *r = nullptr;
    int st = fiso_create(
        plan.dump().c_str(),
        [](void *u, const char *phase, uint64_t d, uint64_t t) {
            auto *q = (P *)u;
            double f = t ? (double)d / (double)t : 0;
            if (f + 1e-12 < q->last) q->monotonic = false;
            q->last = f;
            q->max = std::max(q->max, f);
            if (!strcmp(phase, "encode")) q->encMax = std::max(q->encMax, f);
            if (!strcmp(phase, "verify")) q->verMin = std::min(q->verMin, f);
            return 0;
        },
        &p, &r);
    CHECK_EQ(st, FISO_OK);
    fiso_free(r);
    CHECK(p.max <= 1.0 + 1e-9);
    CHECK(p.monotonic);
    CHECK(p.encMax <= 0.5 + 1e-9);
    CHECK(p.verMin >= 0.5 - 1e-9);
    CHECK(p.last > 0.999);
}

static void testCancel() {
    std::cerr << "[cancellation leaves no partial file]\n";
    json plan = {{"output", W("cancel.fiso")}, {"source", {{"kind", "files"}, {"files", {W("t1.wav")}}}}};
    char *r = nullptr;
    int calls = 0;
    int st = fiso_create(
        plan.dump().c_str(), [](void *u, const char *, uint64_t, uint64_t) { return ++*(int *)u > 3 ? 1 : 0; }, &calls, &r);
    CHECK_EQ(st, FISO_E_CANCELLED);
    CHECK(!pathExists(W("cancel.fiso")));
    int tmp = 0;
    for (auto &e : fs::directory_iterator(g_work))
        if (e.path().extension() == ".tmp") tmp++;
    CHECK_EQ(tmp, 0);
}

static void testDiscId() {
    std::cerr << "[MusicBrainz disc id]\n";
    // reference example from the MusicBrainz Disc ID documentation
    Layout L;
    L.flags = LF_CDDA | LF_HAS_TOC;
    int32_t offs[] = {150, 15363, 32314, 46592, 63414, 80489};
    for (int i = 0; i < 6; i++) {
        TocRec t;
        t.track = (uint8_t)(i + 1);
        t.lba = offs[i] - 150;
        L.toc.push_back(t);
    }
    TocRec lo;
    lo.track = 0xAA;
    lo.lba = 95462 - 150;
    L.toc.push_back(lo);
    std::string toc;
    CHECK_EQ(musicbrainzDiscId(L, &toc), std::string("49HHV7Eb8UKF3aQiNmu1GR8vKTY-"));
    CHECK_EQ(toc, std::string("1 6 95462 150 15363 32314 46592 63414 80489"));
}

int main(int argc, char **argv) {
    g_work = argc > 1 ? argv[1] : "fiso-test-work";
    fs::remove_all(g_work);
    fs::create_directories(g_work);
    auto t0 = std::chrono::steady_clock::now();
    testDiscId();
    testLosslessFiles();
    testCueLayout();
    testPregapSilence();
    testMultiDisc();
    testFlacPassthroughAndEdit();
    testHiRes();
    testCorruption();
    testCdMastering();
    testProgress();
    testCancel();
    testCd();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cerr << "\n" << g_pass << " checks passed, " << g_fail << " failed (" << s << " s)\n";
    return g_fail ? 1 : 0;
}
