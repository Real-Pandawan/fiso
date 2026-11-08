// Portable CD-DA core: TOC/Q/CD-Text parsing, secure ripping with read-offset
// correction, pre-gap detection, AccurateRip.
// SPDX-License-Identifier: Apache-2.0
#include "cd.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

namespace fiso::cd {

static int64_t floorDiv(int64_t a, int64_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
static int bcd(uint8_t v) {
    return (v >> 4) * 10 + (v & 15);
}
static bool bcdOk(uint8_t v) {
    return (v >> 4) < 10 && (v & 15) < 10;
}

int32_t DiscToc::leadout(int session) const {
    for (auto &l : leadouts)
        if (l.first == session) return l.second;
    return -1;
}

DiscToc parseFullToc(const uint8_t *p, size_t n) {
    if (n < 4) fail(FISO_E_FORMAT, "TOC too short");
    size_t len = std::min<size_t>(n, (size_t)((p[0] << 8) | p[1]) + 2);
    DiscToc t;
    for (size_t o = 4; o + 11 <= len; o += 11) {
        const uint8_t *d = p + o;
        uint8_t adr = d[1] >> 4, control = d[1] & 15, point = d[3];
        int32_t lba = ((int32_t)d[8] * 60 + d[9]) * 75 + d[10] - 150;
        if (adr != 1) continue;
        if (point >= 1 && point <= 99) {
            TocTrack tr;
            tr.number = point;
            tr.session = d[0];
            tr.control = control;
            tr.adr = adr;
            tr.lba = lba;
            t.tracks.push_back(tr);
        } else if (point == 0xA2) {
            t.leadouts.push_back({d[0], lba});
        }
    }
    std::sort(t.tracks.begin(), t.tracks.end(), [](auto &a, auto &b) { return a.number < b.number; });
    t.tracks.erase(std::unique(t.tracks.begin(), t.tracks.end(), [](auto &a, auto &b) { return a.number == b.number; }),
                   t.tracks.end());
    if (t.tracks.empty() || t.leadouts.empty()) fail(FISO_E_FORMAT, "TOC without tracks or lead-out");
    return t;
}

static uint16_t crc16q(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

QInfo parseQ(const uint8_t *q) {
    QInfo r;
    r.control = q[0] >> 4;
    r.adr = q[0] & 15;
    uint16_t stored = (uint16_t)(q[10] << 8 | q[11]);
    bool crc = (uint16_t)~crc16q(q, 10) == stored;
    if (!crc) return r;
    if (r.adr != 1) {
        r.ok = true;
        return r;
    }
    if (!bcdOk(q[2])) return r;
    r.track = q[1] == 0xAA ? 0xAA : (bcdOk(q[1]) ? (uint8_t)bcd(q[1]) : 0);
    r.index = (uint8_t)bcd(q[2]);
    r.ok = r.track != 0;
    return r;
}

static std::string latin1ToUtf8(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80)
            o.push_back((char)c);
        else {
            o.push_back((char)(0xC0 | (c >> 6)));
            o.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return o;
}

json parseCdText(const uint8_t *p, size_t n) {
    json out = json::object();
    if (n >= 4 && (n - 4) % 18 == 0 && (size_t)((p[0] << 8) | p[1]) + 2 == n) {
        p += 4;
        n -= 4;
    }
    if (n < 18) return out;
    std::map<int, std::vector<const uint8_t *>> byType;
    int charCode = 0;
    for (size_t o = 0; o + 18 <= n; o += 18) {
        const uint8_t *k = p + o;
        uint16_t crc = (uint16_t)(k[16] << 8 | k[17]);
        if ((uint16_t)~crc16q(k, 16) != crc && crc != 0) continue;  // damaged pack
        int block = (k[3] >> 4) & 7;
        if (block != 0) continue;
        if (k[0] == 0x8F && k[2] == 0) charCode = k[4];
        byType[k[0]].push_back(k);
    }
    if (charCode == 0x80) return out;  // MS-JIS not decoded
    auto strings = [&](int type) {
        std::map<int, std::string> res;
        auto &v = byType[type];
        if (v.empty()) return res;
        std::sort(v.begin(), v.end(), [](auto a, auto b) { return a[2] < b[2]; });
        int track = v[0][1] & 0x7F;
        std::string cur, prev;
        for (auto *k : v) {
            for (int i = 4; i < 16; i++) {
                if (k[i] == 0) {
                    if (cur == "\t") cur = prev;
                    if (!res.count(track)) res[track] = cur;
                    prev = cur;
                    cur.clear();
                    track++;
                } else
                    cur.push_back((char)k[i]);
            }
        }
        return res;
    };
    auto titles = strings(0x80), performers = strings(0x81);
    auto put = [&](const std::string &key, const std::string &v) {
        if (!v.empty()) out[key] = latin1ToUtf8(v);
    };
    if (titles.count(0)) put("album_title", titles[0]);
    if (performers.count(0)) put("album_performer", performers[0]);
    json tracks = json::array();
    std::map<int, json> t;
    for (auto &[k, v] : titles)
        if (k > 0 && !v.empty()) t[k]["title"] = latin1ToUtf8(v);
    for (auto &[k, v] : performers)
        if (k > 0 && !v.empty()) t[k]["performer"] = latin1ToUtf8(v);
    for (auto &[k, v] : t) {
        json j = v;
        j["number"] = k;
        tracks.push_back(j);
    }
    if (!tracks.empty()) out["tracks"] = tracks;
    return out;
}

// ------------------------------------------------------------------ disc analysis
struct AudioDisc {
    std::vector<TocTrack> audio;
    int32_t end = 0;  // first sector after the last audio track
    int session = 1;
    bool has_data = false, mixed_mode_data_first = false;
};

static AudioDisc analyse(const DiscToc &t) {
    AudioDisc a;
    for (auto &tr : t.tracks)
        if (!tr.audio()) a.has_data = true;
    const TocTrack *first = nullptr;
    for (auto &tr : t.tracks)
        if (tr.audio()) {
            first = &tr;
            break;
        }
    if (!first) fail(FISO_E_UNSUPPORTED, "[no_audio] this disc contains no audio tracks");
    a.session = first->session;
    a.mixed_mode_data_first = !t.tracks.front().audio();
    for (size_t i = 0; i < t.tracks.size(); i++) {
        auto &tr = t.tracks[i];
        if (tr.session != a.session) continue;
        if (!tr.audio()) {
            if (!a.audio.empty() && &tr > first) {  // data after audio in the same session
                a.end = tr.lba - 150;               // data track in the same session: its 2-second pregap is not audio
                break;
            }
            continue;
        }
        if (!a.audio.empty() && tr.number != a.audio.back().number + 1)
            fail(FISO_E_UNSUPPORTED, "[mixed_layout] audio tracks are interleaved with data tracks");
        a.audio.push_back(tr);
    }
    if (!a.end) a.end = t.leadout(a.session);
    if (a.end <= a.audio.back().lba) fail(FISO_E_FORMAT, "invalid lead-out in TOC");
    return a;
}

static Layout tocLayout(const DiscToc &t, const AudioDisc &a) {
    Layout l;
    l.flags = LF_CDDA | LF_HAS_TOC;
    l.first_track = (uint8_t)a.audio.front().number;
    l.last_track = (uint8_t)a.audio.back().number;
    l.start_lba = a.audio.front().lba;
    l.total = (uint64_t)(a.end - l.start_lba) * kSectorSamples;
    for (auto &tr : t.tracks) {
        TocRec r;
        r.track = (uint8_t)tr.number;
        r.control = tr.control;
        r.adr = tr.adr;
        r.session = (uint8_t)tr.session;
        r.lba = tr.lba;
        r.stored = tr.audio() && tr.session == a.session ? 1 : 0;
        l.toc.push_back(r);
    }
    for (auto &lo : t.leadouts) {
        TocRec r;
        r.track = 0xAA;
        r.session = (uint8_t)lo.first;
        r.lba = lo.second;
        r.stored = 0;
        l.toc.push_back(r);
    }
    for (auto &tr : a.audio) {
        TrackRec r;
        r.number = (uint8_t)tr.number;
        r.start = (uint64_t)(tr.lba - l.start_lba) * kSectorSamples;
        l.tracks.push_back(r);
    }
    return l;
}

static uint32_t arCddb(const Layout &l) {
    std::vector<int32_t> starts;
    int32_t lead = -1;
    for (auto &t : l.toc) {
        if (t.track == 0xAA)
            lead = std::max(lead, t.lba);
        else
            starts.push_back(t.lba);
    }
    if (starts.empty() || lead < 0) {
        starts.clear();
        for (auto &t : l.tracks) starts.push_back(l.start_lba + (int32_t)(t.start / kSectorSamples));
        lead = l.start_lba + (int32_t)(l.total / kSectorSamples);
    }
    auto digits = [](int v) {
        int s = 0;
        while (v > 0) {
            s += v % 10;
            v /= 10;
        }
        return s;
    };
    int sum = 0;
    for (int32_t s : starts) sum += digits((s + 150) / 75);
    int len = (lead + 150) / 75 - (starts.front() + 150) / 75;
    return ((uint32_t)(sum % 0xFF) << 24) | ((uint32_t)len << 8) | (uint32_t)starts.size();
}

static void arIds(const Layout &l, ArResult &r) {
    r.tracks = (int)l.tracks.size();
    int32_t lead = l.start_lba + (int32_t)(l.total / kSectorSamples);
    for (size_t i = 0; i < l.tracks.size(); i++) {
        int32_t off = l.start_lba + (int32_t)(l.tracks[i].start / kSectorSamples);
        r.id1 += (uint32_t)off;
        r.id2 += (uint32_t)std::max(off, 1) * (uint32_t)(i + 1);
    }
    r.id1 += (uint32_t)lead;
    r.id2 += (uint32_t)lead * (uint32_t)(l.tracks.size() + 1);
    r.cddb = arCddb(l);
    char buf[200];
    snprintf(buf, sizeof buf, "http://www.accuraterip.com/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin", r.id1 & 0xF,
             (r.id1 >> 4) & 0xF, (r.id1 >> 8) & 0xF, r.tracks, r.id1, r.id2, r.cddb);
    r.url = buf;
}

json discInfo(Reader &r) {
    DiscToc t = r.toc();
    AudioDisc a = analyse(t);
    Layout l = tocLayout(t, a);
    json j;
    j["drive"] = r.driveName();
    json tracks = json::array();
    for (auto &tr : t.tracks) {
        int32_t end = 0;
        for (auto &u : t.tracks)
            if (u.number == tr.number + 1 && u.session == tr.session) end = u.lba;
        if (!end) end = t.leadout(tr.session);
        if (tr.audio() && tr.session == a.session && tr.number == a.audio.back().number) end = a.end;
        json x = {{"number", tr.number},
                  {"session", tr.session},
                  {"lba", tr.lba},
                  {"sectors", end - tr.lba},
                  {"seconds", (end - tr.lba) / 75.0},
                  {"audio", tr.audio()},
                  {"pre_emphasis", bool(tr.control & 1)},
                  {"copy_permitted", bool(tr.control & 2)}};
        if (tr.audio() && tr.session == a.session) x["isrc"] = r.isrc(tr.number);
        tracks.push_back(x);
    }
    j["tracks"] = tracks;
    j["audio_tracks"] = a.audio.size();
    j["first_audio_track"] = a.audio.front().number;
    j["audio_end_lba"] = a.end;
    j["seconds"] = (a.end - a.audio.front().lba) / 75.0;
    j["has_data_tracks"] = a.has_data;
    j["mixed_mode"] = a.mixed_mode_data_first;
    j["htoa_sectors"] = a.audio.front().number == 1 ? a.audio.front().lba : 0;
    j["mcn"] = r.mcn();
    auto ct = r.cdText();
    j["cdtext"] = ct.empty() ? json::object() : parseCdText(ct.data(), ct.size());
    std::string toc;
    std::string id = musicbrainzDiscId(l, &toc);
    if (!id.empty()) {
        j["musicbrainz_disc_id"] = id;
        j["musicbrainz_toc"] = toc;
    }
    if (!a.mixed_mode_data_first) {
        ArResult ar;
        arIds(l, ar);
        j["accuraterip"] = {{"id1", ar.id1}, {"id2", ar.id2}, {"cddb", ar.cddb}, {"url", ar.url}};
    }
    return j;
}

// ------------------------------------------------------------------ secure reading
class SectorSource {
  public:
    SectorSource(Reader &r, bool secure, int retries, int32_t lo, int32_t hi)
        : r_(r), secure_(secure), retries_(std::max(2, retries)), lo_(lo), hi_(hi), c2_(r.hasC2()) {}
    std::vector<int32_t> unrecovered;
    uint64_t retried = 0;
    bool c2() const { return c2_; }

    // Fills count sectors starting at `first`; sectors outside [lo, hi) are zero.
    void readRange(int32_t first, int count, uint8_t *out) {
        memset(out, 0, (size_t)count * kSectorBytes);
        int32_t a = std::max(first, lo_), b = std::min(first + count, hi_);
        for (int32_t s = a; s < b;) {
            int k = std::min<int32_t>(26, b - s);
            readBlock(s, k, out + (size_t)(s - first) * kSectorBytes);
            s += k;
        }
    }

  private:
    Reader &r_;
    bool secure_;
    int retries_;
    int32_t lo_, hi_;
    bool c2_;
    std::vector<uint8_t> a_, b_, ca_, cb_, one_, c1_;

    static bool clean(const uint8_t *c2) {
        for (uint32_t i = 0; i < kC2Bytes; i++)
            if (c2[i]) return false;
        return true;
    }
    void bust(int32_t lba) {
        if (hi_ - lo_ < 1000) return;
        int32_t far = lo_ + (int32_t)(((int64_t)(lba - lo_) + (hi_ - lo_) / 2) % (hi_ - lo_));
        uint8_t tmp[kSectorBytes];
        r_.read(far, 1, tmp, nullptr);
    }
    void readBlock(int32_t lba, int k, uint8_t *out) {
        size_t bytes = (size_t)k * kSectorBytes;
        a_.resize(bytes);
        b_.resize(bytes);
        ca_.assign((size_t)k * kC2Bytes, 0);
        cb_.assign((size_t)k * kC2Bytes, 0);
        bool okA = r_.read(lba, k, a_.data(), c2_ ? ca_.data() : nullptr), okB = true;
        if (secure_) {
            bust(lba);
            okB = r_.read(lba, k, b_.data(), c2_ ? cb_.data() : nullptr);
        }
        for (int j = 0; j < k; j++) {
            const uint8_t *sa = a_.data() + (size_t)j * kSectorBytes, *sb = b_.data() + (size_t)j * kSectorBytes;
            bool good = okA && (!c2_ || clean(ca_.data() + (size_t)j * kC2Bytes));
            if (secure_)
                good = good && okB && memcmp(sa, sb, kSectorBytes) == 0 && (!c2_ || clean(cb_.data() + (size_t)j * kC2Bytes));
            if (good)
                memcpy(out + (size_t)j * kSectorBytes, sa, kSectorBytes);
            else
                resolve(lba + j, out + (size_t)j * kSectorBytes, okA ? sa : nullptr, okB && secure_ ? sb : nullptr);
        }
    }
    // Re-reads one sector until two identical error-free reads are obtained.
    void resolve(int32_t lba, uint8_t *out, const uint8_t *a, const uint8_t *b) {
        retried++;
        struct Cand {
            std::vector<uint8_t> data;
            int count;
            bool clean;
        };
        std::vector<Cand> cands;
        auto add = [&](const uint8_t *d, bool cl) -> bool {
            for (auto &c : cands)
                if (memcmp(c.data.data(), d, kSectorBytes) == 0) {
                    c.count++;
                    c.clean = c.clean || cl;
                    return c.count >= (secure_ ? 2 : 1) && c.clean;
                }
            cands.push_back({std::vector<uint8_t>(d, d + kSectorBytes), 1, cl});
            return !secure_ && cl;
        };
        if (a) add(a, !c2_);
        if (b) add(b, !c2_);
        one_.resize(kSectorBytes);
        c1_.assign(kC2Bytes, 0);
        for (int i = 0; i < retries_; i++) {
            bust(lba);
            if (!r_.read(lba, 1, one_.data(), c2_ ? c1_.data() : nullptr)) continue;
            if (add(one_.data(), !c2_ || clean(c1_.data()))) {
                for (auto &c : cands)
                    if (c.count >= (secure_ ? 2 : 1) && c.clean) {
                        memcpy(out, c.data.data(), kSectorBytes);
                        return;
                    }
            }
        }
        unrecovered.push_back(lba);
        const Cand *best = nullptr;
        for (auto &c : cands)
            if (!best || c.count > best->count || (c.count == best->count && c.clean && !best->clean)) best = &c;
        if (best)
            memcpy(out, best->data.data(), kSectorBytes);
        else
            memset(out, 0, kSectorBytes);
    }
};

static QInfo readQRetry(Reader &r, int32_t lba, bool &supported) {
    uint8_t q[16];
    for (int i = 0; i < 3; i++) {
        if (!r.readQ(lba, q)) {
            supported = false;
            return {};
        }
        QInfo x = parseQ(q);
        if (x.ok && x.adr == 1) return x;
    }
    return {};
}

// Returns the LBA of INDEX 00 of `track` (starting at `start`), or start if none.
static int32_t findPregap(Reader &r, int track, int32_t start, int32_t floor_lba, bool &supported) {
    QInfo q = readQRetry(r, start - 1, supported);
    if (!supported || !q.ok || q.track != track || q.index != 0) return start;
    int32_t good = start - 1, step = 75;
    int32_t probe = good - step;
    while (probe > floor_lba) {
        QInfo x = readQRetry(r, probe, supported);
        if (!supported) return start;
        if (x.ok && x.track == track && x.index == 0) {
            good = probe;
            probe -= step;
        } else
            break;
    }
    int32_t lo = std::max(probe, floor_lba), hi = good;  // boundary in (lo, hi]
    while (hi - lo > 1) {
        int32_t mid = lo + (hi - lo) / 2;
        QInfo x = readQRetry(r, mid, supported);
        if (!supported) return start;
        if (x.ok && x.track == track && x.index == 0)
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}

json rip(Reader &r, const json &o, const Progress &prog) {
    std::string output = o.at("output");
    bool overwrite = o.value("overwrite", false);
    if (!overwrite && pathExists(output)) fail(FISO_E_EXISTS, "'" + output + "' already exists");
    bool secure = o.value("mode", std::string("secure")) != "fast";
    int offset = o.value("read_offset", 0);
    if (offset < -5000 || offset > 5000) fail(FISO_E_ARG, "read offset out of range (-5000..5000 samples)");
    std::ostringstream log;
    std::string drive = r.driveName();
    log << "FISO rip log — libfiso " << FISO_LIB_VERSION << "\nDate: " << nowIso8601() << "\nDrive: " << drive
        << "\nMode: " << (secure ? "secure" : "fast") << "\nRead offset correction: " << offset << " samples\n";

    DiscToc t = r.toc();
    AudioDisc a = analyse(t);
    Layout base = tocLayout(t, a);
    std::vector<std::string> warnings;
    if (a.has_data)
        warnings.push_back(
            "[data_tracks] the disc has data tracks; FISO v1.0 stores the audio tracks only (the TOC records the data tracks)");

    // HTOA: audio before track 1 INDEX 01
    int32_t first_lba = a.audio.front().lba, start_lba = first_lba;
    bool htoa = false;
    if (a.audio.front().number == 1 && first_lba > 0 && o.value("read_htoa", true)) {
        std::vector<uint8_t> probe(kSectorBytes);
        if (r.read(0, 1, probe.data(), nullptr)) {
            start_lba = 0;
            htoa = true;
        } else
            warnings.push_back(
                "[htoa_unreadable] the drive cannot read the audio before track 1 (hidden track one audio); it was not stored");
    }
    int32_t end_lba = a.end;
    int32_t read_lo = htoa ? 0 : first_lba;
    log << "Audio: tracks " << a.audio.front().number << "-" << a.audio.back().number << ", LBA " << start_lba << "-" << end_lba
        << (htoa ? " (including hidden track one audio)" : "") << "\n";

    // metadata codes
    std::string mcn = r.mcn();
    if (mcn.size() != 13 || mcn.find_first_not_of("0123456789") != std::string::npos || mcn == "0000000000000") mcn.clear();
    std::map<int, std::string> isrc;
    for (auto &tr : a.audio) {
        std::string s = r.isrc(tr.number);
        bool valid = s.size() == 12 && std::all_of(s.begin(), s.end(), [](char c) { return isalnum((unsigned char)c); }) &&
                     s != "000000000000";
        if (valid) isrc[tr.number] = s;
    }
    std::vector<uint8_t> cdtext = r.cdText();

    // ---- audio extraction with offset correction into a temporary raw file
    std::string tmp = tempPathFor(output) + ".pcm";
    SectorSource src(r, secure, o.value("retries", secure ? 16 : 5), read_lo, end_lba);
    log << "C2 error pointers: " << (src.c2() ? "used" : "not supported by the drive") << "\n";
    uint64_t zero_filled = 0;
    json report;
    try {
        File f = File::open(tmp, "wb");
        const int CH = 24;
        std::vector<uint8_t> raw, outbuf;
        uint32_t total_sectors = (uint32_t)(end_lba - start_lba);
        for (int32_t s = start_lba; s < end_lba; s += CH) {
            int n = std::min<int32_t>(CH, end_lba - s);
            int64_t need0 = (int64_t)s * kSectorSamples + offset, need1 = (int64_t)(s + n) * kSectorSamples + offset;
            int32_t r0 = (int32_t)floorDiv(need0, kSectorSamples);
            int32_t r1 = (int32_t)floorDiv(need1 - 1, kSectorSamples) + 1;
            raw.resize((size_t)(r1 - r0) * kSectorBytes);
            src.readRange(r0, r1 - r0, raw.data());
            size_t skip = (size_t)(need0 - (int64_t)r0 * kSectorSamples) * 4;
            f.write(raw.data() + skip, (size_t)n * kSectorBytes);
            int64_t lo_s = (int64_t)read_lo * kSectorSamples, hi_s = (int64_t)end_lba * kSectorSamples;
            zero_filled += (uint64_t)(std::max<int64_t>(0, std::min(need1, lo_s) - need0) +
                                      std::max<int64_t>(0, need1 - std::max(need0, hi_s)));
            prog("rip", (uint64_t)(s + n - start_lba), total_sectors);
        }
        f.sync();
        f.close();

        // ---- indexes
        bool q_supported = o.value("detect_pregaps", true);
        std::map<int, int32_t> idx00;
        if (q_supported) {
            for (size_t i = 1; i < a.audio.size(); i++) {
                int32_t p = findPregap(r, a.audio[i].number, a.audio[i].lba, a.audio[i - 1].lba + 1, q_supported);
                if (!q_supported) break;
                if (p < a.audio[i].lba) idx00[a.audio[i].number] = p;
                prog("pregaps", i, a.audio.size() - 1);
            }
            if (!q_supported) {
                idx00.clear();
                warnings.push_back(
                    "[no_subchannel] the drive does not return sub-channel Q data: pre-gaps (INDEX 00) could not be detected");
            }
        }
        log << "Pre-gap detection: " << (q_supported ? "sub-channel Q" : "unavailable") << "\n";

        // ---- plan for the image
        json tracks = json::array();
        for (auto &tr : a.audio) {
            json ix = json::array();
            if (tr.number == a.audio.front().number && htoa) ix.push_back({0, 0});
            if (idx00.count(tr.number)) ix.push_back({0, (uint64_t)(idx00[tr.number] - start_lba) * kSectorSamples});
            ix.push_back({1, (uint64_t)(tr.lba - start_lba) * kSectorSamples});
            uint8_t flags = (tr.control & 1 ? 1 : 0) | (tr.control & 2 ? 2 : 0) | (tr.control & 8 ? 4 : 0);
            json tj = {{"number", tr.number}, {"flags", flags}, {"indexes", ix}};
            if (isrc.count(tr.number)) tj["isrc"] = isrc[tr.number];
            tracks.push_back(tj);
            log << "Track " << tr.number << ": LBA " << tr.lba;
            if (idx00.count(tr.number)) log << ", pre-gap " << (tr.lba - idx00[tr.number]) << " sectors";
            if (isrc.count(tr.number)) log << ", ISRC " << isrc[tr.number];
            log << "\n";
        }
        json toc = json::array();
        for (auto &e : base.toc)
            toc.push_back({{"track", e.track},
                           {"control", e.control},
                           {"adr", e.adr},
                           {"session", e.session},
                           {"lba", e.lba},
                           {"stored", e.stored}});

        json meta = o.value("meta", json::object());
        json ct = cdtext.empty() ? json::object() : parseCdText(cdtext.data(), cdtext.size());
        if (!ct.empty()) {
            meta["cdtext"] = ct;
            if (!meta.contains("album") || !meta["album"].is_object()) meta["album"] = json::object();
            if (!meta["album"].contains("title") && ct.contains("album_title")) meta["album"]["title"] = ct["album_title"];
            if (!meta["album"].contains("artist") && ct.contains("album_performer"))
                meta["album"]["artist"] = ct["album_performer"];
            if (!meta.contains("tracks") && ct.contains("tracks")) {
                meta["tracks"] = json::array();
                for (auto &x : ct["tracks"]) {
                    json tj = {{"number", x["number"]}};
                    if (x.contains("title")) tj["title"] = x["title"];
                    if (x.contains("performer")) tj["artist"] = x["performer"];
                    meta["tracks"].push_back(tj);
                }
            }
        }

        log << "Sectors re-read: " << src.retried << "\nUnrecovered sectors: " << src.unrecovered.size() << "\n";
        for (int32_t u : src.unrecovered) log << "  LBA " << u << " (" << msf((uint64_t)u) << ")\n";
        if (zero_filled) log << "Samples outside the readable area (zero-filled by offset correction): " << zero_filled << "\n";
        log << (src.unrecovered.empty() ? "Result: no read error reported\n" : "Result: READ ERRORS REMAIN\n");

        std::string pack_path;
        if (!cdtext.empty()) {
            pack_path = tmp + ".cdtext";
            File pf = File::open(pack_path, "wb");
            pf.write(cdtext.data(), cdtext.size());
        }
        json plan = {
            {"output", output},
            {"overwrite", overwrite},
            {"compression", o.value("compression", 8)},
            {"source",
             {{"kind", "raw"},
              {"path", tmp},
              {"tracks", tracks},
              {"toc", toc},
              {"start_lba", start_lba},
              {"mcn", mcn},
              {"physical", true},
              {"provenance_source", "cd"}}},
            {"meta", meta},
            {"rip",
             {{"mode", secure ? "secure" : "fast"},
              {"drive", drive},
              {"read_offset", offset},
              {"unrecovered_errors", src.unrecovered.size()},
              {"retried_sectors", src.retried},
              {"c2", src.c2()},
              {"verified", false},
              {"verification", ""}}},
            {"log", log.str()},
        };
        if (o.contains("pictures")) plan["pictures"] = o["pictures"];
        if (!pack_path.empty()) plan["cdtext_packs"] = pack_path;
        try {
            report = createImage(plan, prog);
        } catch (...) {
            if (!pack_path.empty()) removeFile(pack_path);
            throw;
        }
        if (!pack_path.empty()) removeFile(pack_path);
        json unrec = json::array();
        for (int32_t u : src.unrecovered) unrec.push_back(u);
        report["rip"] = {{"mode", secure ? "secure" : "fast"},
                         {"drive", drive},
                         {"read_offset", offset},
                         {"c2", src.c2()},
                         {"retried_sectors", src.retried},
                         {"unrecovered_sectors", unrec},
                         {"htoa", htoa},
                         {"pregap_detection", q_supported},
                         {"zero_filled_samples", zero_filled}};
        for (auto &w : warnings) report["warnings"].push_back(w);
        if (!src.unrecovered.empty())
            report["warnings"].push_back("[read_errors] " + std::to_string(src.unrecovered.size()) +
                                         " sector(s) could not be read reliably; the rip is NOT error-free");
    } catch (...) {
        removeFile(tmp);
        throw;
    }
    removeFile(tmp);
    return report;
}

// ------------------------------------------------------------------ AccurateRip
ArResult accurateRip(Image &img, const Progress &prog) {
    const Layout &L = img.layout;
    if (!L.cdda()) fail(FISO_E_UNSUPPORTED, "[not_cdda] AccurateRip needs a CD-DA image");
    if (L.flags & LF_HAS_TOC) {
        const TocRec *first = nullptr;
        for (auto &t : L.toc)
            if (t.track != 0xAA && (!first || t.track < first->track)) first = &t;
        if (first && (first->control & 4))
            fail(FISO_E_UNSUPPORTED, "[mixed_mode] AccurateRip is not supported for discs starting with a data track");
    }
    ArResult r;
    arIds(L, r);
    size_t n = L.tracks.size();
    std::vector<int32_t> buf(16384 * 2);
    uint64_t done = 0;
    for (size_t i = 0; i < n; i++) {
        auto &t = L.tracks[i];
        uint64_t from = i == 0 ? 588 * 5 : 0;
        uint64_t to = i + 1 == n ? (t.length > 588 * 5 ? t.length - 588 * 5 : 0) : t.length;
        uint32_t lo = 0, hi = 0;
        uint64_t mul = 1;
        for (uint64_t p = 0; p < t.length;) {
            size_t k = (size_t)std::min<uint64_t>(16384, t.length - p);
            if (img.readPcm(t.start + p, k, buf.data()) != k) fail(FISO_E_INTEGRITY, "audio shorter than expected");
            for (size_t j = 0; j < k; j++, mul++) {
                if (mul < from || mul > to) continue;
                uint32_t w = (uint32_t)(uint16_t)buf[2 * j] | ((uint32_t)(uint16_t)buf[2 * j + 1] << 16);
                uint64_t prod = (uint64_t)w * (uint32_t)mul;
                lo += (uint32_t)prod;
                hi += (uint32_t)(prod >> 32);
            }
            p += k;
            done += k;
            prog("accuraterip", done, L.total);
        }
        r.crcs.push_back({t.number, lo, lo + hi});
    }
    return r;
}

json accurateRipCompare(const ArResult &r, const std::vector<uint8_t> &db) {
    struct Hit {
        uint32_t conf_v1 = 0, conf_v2 = 0;
    };
    std::vector<Hit> hits(r.crcs.size());
    size_t pos = 0;
    int entries = 0;
    auto le32 = [&](size_t p) {
        return (uint32_t)db[p] | (uint32_t)db[p + 1] << 8 | (uint32_t)db[p + 2] << 16 | (uint32_t)db[p + 3] << 24;
    };
    while (pos + 13 <= db.size()) {
        int cnt = db[pos];
        uint32_t id1 = le32(pos + 1), id2 = le32(pos + 5), cddb = le32(pos + 9);
        pos += 13;
        if (pos + (size_t)cnt * 9 > db.size()) break;
        bool ours = cnt == r.tracks && id1 == r.id1 && id2 == r.id2 && cddb == r.cddb;
        for (int i = 0; i < cnt; i++, pos += 9) {
            if (!ours) continue;
            uint32_t conf = db[pos], crc = le32(pos + 1);
            if (crc == r.crcs[i].v2) hits[i].conf_v2 = std::max(hits[i].conf_v2, conf);
            if (crc == r.crcs[i].v1) hits[i].conf_v1 = std::max(hits[i].conf_v1, conf);
        }
        if (ours) entries++;
    }
    json tracks = json::array();
    bool all = !r.crcs.empty() && entries > 0;
    uint32_t minconf = UINT32_MAX;
    for (size_t i = 0; i < r.crcs.size(); i++) {
        char v1[9], v2[9];
        snprintf(v1, sizeof v1, "%08x", r.crcs[i].v1);
        snprintf(v2, sizeof v2, "%08x", r.crcs[i].v2);
        json t = {{"number", r.crcs[i].number}, {"v1", v1}, {"v2", v2}};
        if (hits[i].conf_v2) {
            t["match"] = "v2";
            t["confidence"] = hits[i].conf_v2;
        } else if (hits[i].conf_v1) {
            t["match"] = "v1";
            t["confidence"] = hits[i].conf_v1;
        } else {
            t["match"] = nullptr;
            all = false;
        }
        if (t.contains("confidence")) minconf = std::min<uint32_t>(minconf, t["confidence"].get<uint32_t>());
        tracks.push_back(t);
    }
    return {{"in_database", entries > 0},
            {"entries", entries},
            {"tracks", tracks},
            {"all_matched", all},
            {"min_confidence", minconf == UINT32_MAX ? 0 : minconf}};
}

}  // namespace fiso::cd
