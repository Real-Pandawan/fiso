// Tests of the portable CD core against a simulated drive: TOC/Q/CD-Text parsing,
// secure ripping with read-offset correction and read errors, pre-gap detection,
// HTOA and AccurateRip.
// SPDX-License-Identifier: Apache-2.0
#include "cd.h"
#include "cd_mmc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

using namespace fiso;
namespace fs = std::filesystem;

extern int g_fail, g_pass;
extern std::string g_work;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (c)                                                              \
            g_pass++;                                                       \
        else {                                                              \
            g_fail++;                                                       \
            std::cerr << "  FAIL test_cd.cpp:" << __LINE__ << "  " #c "\n"; \
        }                                                                   \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                \
    do {                                                                                                              \
        auto _a = (a);                                                                                                \
        auto _b = (b);                                                                                                \
        if (_a == _b)                                                                                                 \
            g_pass++;                                                                                                 \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            std::cerr << "  FAIL test_cd.cpp:" << __LINE__ << "  " #a " == " #b "  (" << _a << " vs " << _b << ")\n"; \
        }                                                                                                             \
    } while (0)

static std::string W(const std::string &n) {
    return (fs::path(g_work) / n).string();
}

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
static uint8_t toBcd(int v) {
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

struct SimTrack {
    int number;
    int32_t lba;
    uint8_t control = 0;
    int32_t idx0 = -1;
    std::string isrc;
};

// A drive reading a disc whose true audio is `pcm` (stereo int16, LBA 0 = sample 0).
class SimDrive : public cd::Reader {
  public:
    std::vector<int16_t> pcm;
    std::vector<SimTrack> tracks;
    int32_t leadout = 0;
    int offset = 0;  // drive read offset: raw(x) = true(x - offset)
    bool c2 = true, q = true, htoa_readable = true;
    std::map<int32_t, int> flaky;  // lba -> number of bad reads before good ones
    std::set<int32_t> dead;        // never read correctly
    std::string mcnv = "0123456789012";
    std::vector<uint8_t> text;
    int reads = 0;

    std::string driveName() override { return "SIMULATED DRIVE 1.0"; }
    cd::DiscToc toc() override {
        // parse a real full-TOC byte stream, to exercise the parser
        auto b = fullToc();
        return cd::parseFullToc(b.data(), b.size());
    }
    // READ TOC format 2 (full TOC, MSF) as a drive returns it
    std::vector<uint8_t> fullToc() {
        std::vector<uint8_t> b(4);
        auto desc = [&](uint8_t point, uint8_t ctrl, int32_t lba, int pm = -1) {
            int32_t a = lba + 150;
            uint8_t d[11] = {1,
                             (uint8_t)(0x10 | ctrl),
                             0,
                             point,
                             0,
                             0,
                             0,
                             0,
                             (uint8_t)(pm >= 0 ? pm : a / 4500),
                             (uint8_t)(pm >= 0 ? 0 : a / 75 % 60),
                             (uint8_t)(pm >= 0 ? 0 : a % 75)};
            b.insert(b.end(), d, d + 11);
        };
        desc(0xA0, 0, 0, tracks.front().number);
        desc(0xA1, 0, 0, tracks.back().number);
        desc(0xA2, 0, leadout);
        for (auto &t : tracks) desc((uint8_t)t.number, t.control, t.lba);
        size_t len = b.size() - 2;
        b[0] = (uint8_t)(len >> 8);
        b[1] = (uint8_t)len;
        b[2] = 1;
        b[3] = 1;
        return b;
    }
    bool hasC2() override { return c2; }
    int16_t sample(int64_t frame, int ch) {
        int64_t x = frame - offset;
        if (x < 0 || x >= (int64_t)leadout * 588) return 0;
        return pcm[(size_t)x * 2 + ch];
    }
    bool read(int32_t lba, int n, uint8_t *audio, uint8_t *c2b) override {
        reads++;
        if (lba < 0 || lba + n > leadout) return false;
        if (!htoa_readable && lba < tracks.front().lba) return false;
        for (int i = 0; i < n; i++) {
            int32_t s = lba + i;
            uint8_t *o = audio + (size_t)i * 2352;
            for (int k = 0; k < 588; k++) {
                int64_t f = (int64_t)s * 588 + k;
                int16_t l = sample(f, 0), r = sample(f, 1);
                o[4 * k] = (uint8_t)l;
                o[4 * k + 1] = (uint8_t)(l >> 8);
                o[4 * k + 2] = (uint8_t)r;
                o[4 * k + 3] = (uint8_t)(r >> 8);
            }
            bool bad = dead.count(s) || (flaky.count(s) && flaky[s] > 0);
            if (flaky.count(s) && flaky[s] > 0) flaky[s]--;
            if (bad) {
                o[100] ^= (uint8_t)(0x55 + reads);  // a different wrong value on every read
                if (c2b && !dead.count(s)) c2b[(size_t)i * 294 + 12] = 0x80;
            }
            if (c2b && !bad) memset(c2b + (size_t)i * 294, 0, 294);
            if (c2b && dead.count(s)) memset(c2b + (size_t)i * 294, 0, 294);  // the worst case: errors not flagged
        }
        return true;
    }
    bool readQ(int32_t lba, uint8_t *o) override {
        if (!q) return false;
        int tn = 0, idx = 1;
        for (auto &t : tracks) {
            if (t.idx0 >= 0 && lba >= t.idx0 && lba < t.lba) {
                tn = t.number;
                idx = 0;
            }
            if (lba >= t.lba) {
                tn = t.number;
                idx = 1;
            }
        }
        if (!tn) {
            tn = tracks.front().number;
            idx = 0;
        }
        memset(o, 0, 16);
        o[0] = 0x01;
        o[1] = toBcd(tn);
        o[2] = toBcd(idx);
        uint16_t c = (uint16_t)~crc16(o, 10);
        o[10] = (uint8_t)(c >> 8);
        o[11] = (uint8_t)c;
        return true;
    }
    std::string isrc(int track) override {
        for (auto &t : tracks)
            if (t.number == track) return t.isrc;
        return "";
    }
    std::string mcn() override { return mcnv; }
    std::vector<uint8_t> cdText() override { return text; }
};

// The same disc seen through MMC commands, as the Linux and Windows adapters see a real drive:
// READ TOC (43h), READ CD (BEh) with C2 pointers and raw P-W sub-channel, READ SUB-CHANNEL (42h).
class SimMmcDrive : public cd::MmcTransport {
  public:
    explicit SimMmcDrive(SimDrive &d) : d_(d) {}
    int commands = 0;

    long command(const uint8_t *cdb, size_t, uint8_t *data, size_t dataLen) override {
        commands++;
        switch (cdb[0]) {
            case 0x43:
                return readToc(cdb, data, dataLen);
            case 0xBE:
                return readCd(cdb, data, dataLen);
            case 0x42:
                return readSubChannel(cdb, data, dataLen);
            default:
                return -1;
        }
    }

  private:
    static long reply(const std::vector<uint8_t> &r, size_t alloc, uint8_t *data, size_t dataLen) {
        size_t n = std::min({r.size(), alloc, dataLen});
        std::memcpy(data, r.data(), n);
        return (long)n;
    }
    long readToc(const uint8_t *cdb, uint8_t *data, size_t dataLen) {
        size_t alloc = (size_t)(cdb[7] << 8 | cdb[8]);
        int format = cdb[2] & 0x0F;
        if (format == 2) return reply(d_.fullToc(), alloc, data, dataLen);
        if (format == 5 && !d_.text.empty()) {
            std::vector<uint8_t> r(4);
            r.insert(r.end(), d_.text.begin(), d_.text.end());
            r[0] = (uint8_t)((r.size() - 2) >> 8);
            r[1] = (uint8_t)(r.size() - 2);
            return reply(r, alloc, data, dataLen);
        }
        return -1;
    }
    long readCd(const uint8_t *cdb, uint8_t *data, size_t dataLen) {
        int32_t lba = (int32_t)((uint32_t)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5]);
        int n = cdb[6] << 16 | cdb[7] << 8 | cdb[8];
        bool withC2 = cdb[9] & 0x02, withPW = cdb[10] == 0x01;
        if (cdb[1] != 0x04 || !(cdb[9] & 0x10) || (withC2 && !d_.c2)) return -1;
        size_t bs = cd::kSectorBytes + (withC2 ? cd::kC2Bytes : 0) + (withPW ? 96 : 0);
        if ((size_t)n * bs > dataLen) return -1;
        std::vector<uint8_t> audio((size_t)n * cd::kSectorBytes), c2((size_t)n * cd::kC2Bytes);
        if (!d_.read(lba, n, audio.data(), withC2 ? c2.data() : nullptr)) return -1;
        for (int i = 0; i < n; i++) {
            uint8_t *o = data + (size_t)i * bs;
            std::memcpy(o, audio.data() + (size_t)i * cd::kSectorBytes, cd::kSectorBytes);
            o += cd::kSectorBytes;
            if (withC2) {
                std::memcpy(o, c2.data() + (size_t)i * cd::kC2Bytes, cd::kC2Bytes);
                o += cd::kC2Bytes;
            }
            if (withPW) {
                uint8_t q[16];
                if (!d_.readQ(lba + i, q)) return -1;
                for (int b = 0; b < 96; b++) o[b] = (uint8_t)(0x80 | 0x15 | ((q[b / 8] >> (7 - b % 8)) & 1) << 6);
            }
        }
        return (long)((size_t)n * bs);
    }
    long readSubChannel(const uint8_t *cdb, uint8_t *data, size_t dataLen) {
        std::vector<uint8_t> r(24, 0);
        std::string v = cdb[3] == 0x02 ? d_.mcn() : cdb[3] == 0x03 ? d_.isrc(cdb[6]) : "";
        if (cdb[3] != 0x02 && cdb[3] != 0x03) return -1;
        r[3] = 20;
        r[4] = cdb[3];
        if (!v.empty()) {
            r[8] = 0x80;
            std::memcpy(r.data() + 9, v.data(), std::min<size_t>(v.size(), 13));
        }
        return reply(r, (size_t)(cdb[7] << 8 | cdb[8]), data, dataLen);
    }
    SimDrive &d_;
};

static std::vector<uint8_t> makeCdText(const std::vector<std::pair<int, std::vector<std::string>>> &types) {
    std::vector<uint8_t> out;
    int seq = 0;
    for (auto &[type, strs] : types) {
        std::string blob;
        for (auto &s : strs) {
            blob += s;
            blob.push_back('\0');
        }
        int track = 0;
        size_t pos = 0;
        while (pos < blob.size()) {
            uint8_t p[18] = {(uint8_t)type, (uint8_t)track, (uint8_t)seq++, 0};
            for (int i = 0; i < 12; i++) {
                p[4 + i] = pos < blob.size() ? (uint8_t)blob[pos] : 0;
                if (pos < blob.size() && blob[pos] == '\0') track++;
                pos++;
            }
            uint16_t c = (uint16_t)~crc16(p, 16);
            p[16] = (uint8_t)(c >> 8);
            p[17] = (uint8_t)c;
            out.insert(out.end(), p, p + 18);
        }
    }
    uint8_t sz[18] = {0x8F, 0, (uint8_t)seq++, 0, 0x00};
    uint16_t c = (uint16_t)~crc16(sz, 16);
    sz[16] = (uint8_t)(c >> 8);
    sz[17] = (uint8_t)c;
    out.insert(out.end(), sz, sz + 18);
    return out;
}

static SimDrive makeDisc() {
    SimDrive d;
    d.leadout = 2600;
    d.pcm.resize((size_t)d.leadout * 588 * 2);
    double ph = 0;
    for (size_t i = 0; i < d.pcm.size() / 2; i++) {
        double v = std::sin(ph) * 9000 + std::sin(ph * 0.37) * 3000;
        ph += 2 * 3.14159265358979 * 523.25 / 44100;
        d.pcm[2 * i] = (int16_t)v;
        d.pcm[2 * i + 1] = (int16_t)(-v / 2 + (int)(i % 7));
    }
    // HTOA: audio before track 1 (LBA 0-299), track 2 with a 150-sector pre-gap, track 3 without
    d.tracks = {{1, 300, 0, 0, "FRXXX2600011"}, {2, 1100, 1, 950, "FRXXX2600012"}, {3, 1900, 2, -1, ""}};
    return d;
}

static std::vector<uint8_t> latin1Text() {
    auto l1 = [](const std::string &u) {
        std::string o;
        for (size_t i = 0; i < u.size(); i++) {
            unsigned char c = u[i];
            if (c == 0xC3 && i + 1 < u.size()) {
                o.push_back((char)(0x40 + (unsigned char)u[++i]));
            } else
                o.push_back((char)c);
        }
        return o;
    };
    return makeCdText({{0x80, {l1("Disque simulé"), l1("Première"), l1("Deuxième"), "Troisieme"}},
                       {0x81, {"Orchestre", "\t", "Invite", "\t"}}});
}

static uint32_t refArCrc(const std::vector<int16_t> &pcm, uint64_t start, uint64_t len, bool first, bool last, bool v2) {
    // straight transcription of whipper's compute_checksums()
    uint32_t lo = 0, hi = 0;
    uint32_t from = first ? (2352 * 5) / 4 : 0, to = (uint32_t)len - (last ? (2352 * 5) / 4 : 0);
    uint32_t mul = 1;
    for (uint64_t i = 0; i < len; i++) {
        if (mul >= from && mul <= to) {
            uint32_t w = (uint16_t)pcm[2 * (start + i)] | ((uint32_t)(uint16_t)pcm[2 * (start + i) + 1] << 16);
            uint64_t p = (uint64_t)w * mul;
            hi += (uint32_t)(p >> 32);
            lo += (uint32_t)p;
        }
        mul++;
    }
    return v2 ? lo + hi : lo;
}

void testCd() {
    std::cerr << "[CD: TOC, Q and CD-Text parsers]\n";
    {
        SimDrive d = makeDisc();
        auto t = d.toc();
        CHECK_EQ(t.tracks.size(), (size_t)3);
        CHECK_EQ(t.tracks[1].lba, 1100);
        CHECK(t.tracks[1].control == 1 && t.tracks[2].control == 2);
        CHECK_EQ(t.leadout(1), 2600);
        uint8_t q[16];
        d.readQ(1000, q);
        auto qi = cd::parseQ(q);
        CHECK(qi.ok && qi.track == 2 && qi.index == 0);
        q[3] ^= 1;
        CHECK(!cd::parseQ(q).ok);  // CRC protects the frame
        auto txt = latin1Text();
        json ct = cd::parseCdText(txt.data(), txt.size());
        CHECK_EQ(ct.value("album_title", ""), std::string("Disque simulé"));
        CHECK_EQ(ct.value("album_performer", ""), std::string("Orchestre"));
        CHECK_EQ(ct["tracks"][0]["title"].get<std::string>(), std::string("Première"));
        CHECK_EQ(ct["tracks"][0]["performer"].get<std::string>(), std::string("Orchestre"));  // TAB = repeat previous
        CHECK_EQ(ct["tracks"][1]["performer"].get<std::string>(), std::string("Invite"));
    }

    std::cerr << "[CD rip: secure mode, offset +667, HTOA, pre-gaps, ISRC, MCN, CD-Text, flaky sectors]\n";
    SimDrive d = makeDisc();
    d.text = latin1Text();
    d.offset = 667;
    d.flaky = {{500, 3}, {1500, 1}, {2000, 5}};
    json info = cd::discInfo(d);
    CHECK_EQ(info["audio_tracks"].get<int>(), 3);
    CHECK_EQ(info["htoa_sectors"].get<int>(), 300);
    CHECK(info.contains("musicbrainz_disc_id"));
    CHECK(info["accuraterip"]["url"].get<std::string>().find("dBAR-003-") != std::string::npos);
    json o = {
        {"output", W("rip.fiso")}, {"mode", "secure"}, {"read_offset", 667}, {"meta", {{"disc", {{"number", 1}, {"total", 1}}}}}};
    json rep = cd::rip(d, o, Progress());
    CHECK(rep["verify"]["intact_audio"]["ok"] == true);
    CHECK(rep["rip"]["unrecovered_sectors"].empty());
    CHECK(rep["rip"]["retried_sectors"].get<uint64_t>() >= 3);
    CHECK(rep["rip"]["htoa"] == true);
    auto img = Image::open(W("rip.fiso"));
    const Layout &L = img->layout;
    CHECK(L.flags & LF_HTOA);
    CHECK(L.flags & LF_PHYSICAL);
    CHECK(L.cdda());
    CHECK_EQ(L.tracks.size(), (size_t)3);
    CHECK_EQ(L.tracks[0].start, (uint64_t)300 * 588);
    CHECK_EQ(L.tracks[1].start, (uint64_t)1100 * 588);
    auto *i0 = L.findIndex(2, 0);
    CHECK(i0 && i0->pos == (uint64_t)950 * 588);
    CHECK(L.findIndex(3, 0) == nullptr);
    CHECK_EQ(L.tracks[0].isrc, std::string("FRXXX2600011"));
    CHECK_EQ(L.tracks[1].flags, (uint8_t)1);  // pre-emphasis
    CHECK_EQ(L.tracks[2].flags, (uint8_t)2);  // copy permitted
    CHECK_EQ(L.mcn, std::string("0123456789012"));
    CHECK_EQ(img->meta["album"]["title"].get<std::string>(), std::string("Disque simulé"));
    CHECK_EQ(img->meta["tracks"][1]["title"].get<std::string>(), std::string("Deuxième"));
    CHECK_EQ(img->meta["provenance"]["rip"]["read_offset"].get<int>(), 667);
    bool cdtx = false, rlog = false;
    for (auto &c : img->chunks) {
        cdtx |= c.type == "CDTX";
        rlog |= c.type == "RLOG";
    }
    CHECK(cdtx && rlog);
    // offset-corrected audio equals the true audio, except the final 667 samples the drive cannot reach
    std::vector<int32_t> got(L.total * 2);
    img->readPcm(0, L.total, got.data());
    size_t bad = 0, tail = 0;
    for (uint64_t s = 0; s < L.total; s++) {
        bool same = got[2 * s] == d.pcm[2 * s] && got[2 * s + 1] == d.pcm[2 * s + 1];
        if (s < L.total - 667)
            bad += !same;
        else
            tail += got[2 * s] == 0 && got[2 * s + 1] == 0;
    }
    CHECK_EQ(bad, (size_t)0);
    CHECK_EQ(tail, (size_t)667);
    CHECK_EQ(rep["rip"]["zero_filled_samples"].get<uint64_t>(), (uint64_t)667);

    std::cerr << "[CD rip through MMC commands (Linux and Windows drives)]\n";
    {
        uint8_t q[16], pw[96], back[16];
        d.readQ(1000, q);
        for (int b = 0; b < 96; b++) pw[b] = (uint8_t)(0x3F | ((q[b / 8] >> (7 - b % 8)) & 1) << 6);
        cd::qFromRawPW(pw, back);
        CHECK(std::memcmp(q, back, 12) == 0);  // only bit 6 (Q) is taken from the raw P-W bytes

        SimDrive m = makeDisc();
        m.text = latin1Text();
        m.offset = 667;
        m.flaky = {{500, 3}, {1500, 1}, {2000, 5}};
        SimMmcDrive bus(m);
        cd::MmcReader reader("SIMULATED MMC DRIVE", bus);
        reader.checkAudioDisc();
        CHECK(reader.hasC2());
        CHECK_EQ(reader.mcn(), std::string("0123456789012"));
        CHECK_EQ(reader.isrc(2), std::string("FRXXX2600012"));
        CHECK_EQ(reader.isrc(3), std::string(""));
        CHECK(reader.cdText() == m.text);
        json mi = cd::discInfo(reader);
        CHECK(mi["musicbrainz_disc_id"] == info["musicbrainz_disc_id"]);
        CHECK(mi["accuraterip"]["url"] == info["accuraterip"]["url"]);
        json mo = o;
        mo["output"] = W("rip-mmc.fiso");
        json mr = cd::rip(reader, mo, Progress());
        CHECK(mr["verify"]["intact_audio"]["ok"] == true);
        CHECK(mr["rip"]["unrecovered_sectors"].empty());
        CHECK(mr["rip"]["retried_sectors"].get<uint64_t>() >= 3);
        auto mimg = Image::open(W("rip-mmc.fiso"));
        const Layout &ML = mimg->layout;
        CHECK_EQ(ML.total, L.total);
        CHECK(ML.findIndex(2, 0) && ML.findIndex(2, 0)->pos == (uint64_t)950 * 588);
        CHECK_EQ(ML.tracks[1].isrc, std::string("FRXXX2600012"));
        CHECK_EQ(ML.tracks[1].flags, (uint8_t)1);
        CHECK_EQ(ML.mcn, std::string("0123456789012"));
        CHECK_EQ(mimg->meta["tracks"][0]["title"].get<std::string>(), std::string("Première"));
        std::vector<int32_t> mg(ML.total * 2);
        mimg->readPcm(0, ML.total, mg.data());
        CHECK(mg == got);  // bit-identical to the rip through the direct reader

        SimDrive nc = makeDisc();
        nc.c2 = false;
        SimMmcDrive bus2(nc);
        cd::MmcReader r2("NO C2", bus2);
        CHECK(!r2.hasC2());
        CHECK(r2.cdText().empty());
        json f = cd::rip(r2, {{"output", W("rip-mmc-fast.fiso")}, {"mode", "fast"}}, Progress());
        CHECK(f["rip"]["unrecovered_sectors"].empty());
    }

    std::cerr << "[CD rip: unrecoverable sector is reported, never hidden]\n";
    {
        SimDrive e = makeDisc();
        e.dead = {1234};
        json r2 = cd::rip(e, {{"output", W("rip-bad.fiso")}, {"mode", "secure"}, {"retries", 4}}, Progress());
        CHECK_EQ(r2["rip"]["unrecovered_sectors"].size(), (size_t)1);
        CHECK_EQ(r2["rip"]["unrecovered_sectors"][0].get<int>(), 1234);
        bool warned = false;
        for (auto &w : r2["warnings"]) warned |= w.get<std::string>().find("[read_errors]") != std::string::npos;
        CHECK(warned);
        auto im = Image::open(W("rip-bad.fiso"));
        CHECK_EQ(im->meta["provenance"]["rip"]["unrecovered_errors"].get<int>(), 1);
        CHECK(im->meta["provenance"]["rip"]["verified"] == false);
    }
    std::cerr << "[CD rip: fast mode, no C2, no sub-channel Q, HTOA unreadable]\n";
    {
        SimDrive e = makeDisc();
        e.c2 = false;
        e.q = false;
        e.htoa_readable = false;
        json r3 = cd::rip(e, {{"output", W("rip-fast.fiso")}, {"mode", "fast"}}, Progress());
        std::string ws = r3["warnings"].dump();
        CHECK(ws.find("[no_subchannel]") != std::string::npos);
        CHECK(ws.find("[htoa_unreadable]") != std::string::npos);
        auto im = Image::open(W("rip-fast.fiso"));
        CHECK(!(im->layout.flags & LF_HTOA));
        CHECK_EQ(im->layout.start_lba, 300);
        CHECK(im->layout.findIndex(2, 0) == nullptr);
    }

    std::cerr << "[AccurateRip checksums and database matching]\n";
    {
        auto ar = cd::accurateRip(*img, Progress());
        CHECK_EQ(ar.tracks, 3);
        for (size_t k = 0; k < 3; k++) {
            auto &t = L.tracks[k];
            std::vector<int16_t> pcm16(got.begin(), got.end());
            CHECK_EQ(ar.crcs[k].v1, refArCrc(pcm16, t.start, t.length, k == 0, k == 2, false));
            CHECK_EQ(ar.crcs[k].v2, refArCrc(pcm16, t.start, t.length, k == 0, k == 2, true));
        }
        auto dbEntry = [&](bool good) {
            std::vector<uint8_t> db;
            auto le = [&](uint32_t v) {
                for (int i = 0; i < 4; i++) db.push_back((uint8_t)(v >> (8 * i)));
            };
            db.push_back(3);
            le(ar.id1);
            le(ar.id2);
            le(ar.cddb);
            for (auto &c : ar.crcs) {
                db.push_back(17);
                le(good ? c.v2 : c.v2 ^ 1);
                le(0);
            }
            return db;
        };
        json m = cd::accurateRipCompare(ar, dbEntry(true));
        CHECK(m["all_matched"] == true);
        CHECK_EQ(m["min_confidence"].get<int>(), 17);
        CHECK(cd::accurateRipCompare(ar, dbEntry(false))["all_matched"] == false);
        CHECK(cd::accurateRipCompare(ar, {})["in_database"] == false);
        // the image's disc ids are those of the physical TOC
        CHECK(ar.url == info["accuraterip"]["url"].get<std::string>());
    }
}
