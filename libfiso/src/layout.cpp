// LAYT / PICT / META model — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <cstring>
#include <set>

namespace fiso {

const IndexRec *Layout::findIndex(uint8_t track, uint8_t idx) const {
    for (auto &i : indexes)
        if (i.track == track && i.index == idx) return &i;
    return nullptr;
}

std::vector<uint8_t> serializeLayout(const Layout &l) {
    std::vector<uint8_t> v;
    put16(v, 1);
    put16(v, l.flags);
    put32(v, l.rate);
    v.push_back(l.channels);
    v.push_back(l.bps);
    put16(v, 0);
    put16(v, (uint16_t)l.tracks.size());
    put16(v, (uint16_t)l.indexes.size());
    put16(v, (uint16_t)l.segments.size());
    put16(v, (uint16_t)l.toc.size());
    v.push_back(l.first_track);
    v.push_back(l.last_track);
    put16(v, 0);
    put32(v, (uint32_t)l.start_lba);
    put32(v, 0);
    put64(v, l.total);
    v.insert(v.end(), l.timeline_sha.begin(), l.timeline_sha.end());
    putstr(v, l.mcn, 16);
    putz(v, 40);
    for (auto &t : l.tracks) {
        v.push_back(t.number);
        v.push_back(t.flags);
        put16(v, t.first_index);
        put16(v, t.index_count);
        put16(v, 0);
        put64(v, t.start);
        put64(v, t.length);
        putstr(v, t.isrc, 16);
        put64(v, 0);
    }
    for (auto &i : l.indexes) {
        v.push_back(i.track);
        v.push_back(i.index);
        putz(v, 6);
        put64(v, i.pos);
    }
    for (auto &s : l.segments) {
        put32(v, s.chunk_index);
        v.push_back(s.kind);
        v.push_back(s.track);
        put16(v, 0);
        put64(v, s.start);
        put64(v, s.length);
        v.insert(v.end(), s.sha.begin(), s.sha.end());
        put64(v, 0);
    }
    for (auto &t : l.toc) {
        v.push_back(t.track);
        v.push_back(t.control);
        v.push_back(t.adr);
        v.push_back(t.session);
        put32(v, (uint32_t)t.lba);
        v.push_back(t.stored);
        putz(v, 7);
    }
    return v;
}

static void bad(const std::string &m) {
    fail(FISO_E_FORMAT, "LAYT: " + m);
}

bool computeCddaFlag(const Layout &l) {
    if (l.rate != 44100 || l.channels != 2 || l.bps != 16) return false;
    auto al = [](uint64_t x) { return x % kSectorSamples == 0; };
    if (!al(l.total)) return false;
    for (auto &t : l.tracks)
        if (!al(t.start) || !al(t.length)) return false;
    for (auto &i : l.indexes)
        if (!al(i.pos)) return false;
    for (auto &s : l.segments)
        if (!al(s.start) || !al(s.length)) return false;
    return true;
}

Layout parseLayout(const uint8_t *p, size_t n) {
    if (n < 128) bad("chunk too small");
    Layout l;
    if (get16(p) != 1) fail(FISO_E_UNSUPPORTED, "LAYT: unsupported layout version");
    l.flags = get16(p + 2);
    l.rate = get32(p + 4);
    l.channels = p[8];
    l.bps = p[9];
    uint32_t tc = get16(p + 12), ic = get16(p + 14), sc = get16(p + 16), cc = get16(p + 18);
    l.first_track = p[20];
    l.last_track = p[21];
    l.start_lba = (int32_t)get32(p + 24);
    l.total = get64(p + 32);
    memcpy(l.timeline_sha.data(), p + 40, 32);
    l.mcn = getstr(p + 72, 16);

    if (l.rate < 1 || l.rate > 655350) bad("invalid sample rate");
    if (l.channels < 1 || l.channels > 8) bad("invalid channel count");
    if (l.bps < 4 || l.bps > 32) bad("invalid bits per sample");
    if (tc < 1 || tc > 99) bad("invalid track count");
    if (ic < tc || sc < tc || sc > tc + 1 || cc > 101) bad("invalid record counts");
    if (l.total == 0 || l.total > (1ull << 40)) bad("invalid total samples");
    uint64_t need = 128ull + 48ull * tc + 16ull * ic + 64ull * sc + 16ull * cc;
    if (n != need) bad("chunk length does not match record counts");
    if (l.first_track < 1 || l.last_track != l.first_track + tc - 1 || l.last_track > 99) bad("invalid track numbering");
    if (!l.mcn.empty() && (l.mcn.size() != 13 || l.mcn.find_first_not_of("0123456789") != std::string::npos)) bad("invalid MCN");

    const uint8_t *q = p + 128;
    for (uint32_t i = 0; i < tc; i++, q += 48) {
        TrackRec t;
        t.number = q[0];
        t.flags = q[1];
        t.first_index = get16(q + 2);
        t.index_count = get16(q + 4);
        t.start = get64(q + 8);
        t.length = get64(q + 16);
        t.isrc = getstr(q + 24, 16);
        l.tracks.push_back(t);
    }
    for (uint32_t i = 0; i < ic; i++, q += 16) l.indexes.push_back({q[0], q[1], get64(q + 8)});
    for (uint32_t i = 0; i < sc; i++, q += 64) {
        SegmentRec s;
        s.chunk_index = get32(q);
        s.kind = q[4];
        s.track = q[5];
        s.start = get64(q + 8);
        s.length = get64(q + 16);
        memcpy(s.sha.data(), q + 24, 32);
        l.segments.push_back(s);
    }
    for (uint32_t i = 0; i < cc; i++, q += 16) {
        TocRec t;
        t.track = q[0];
        t.control = q[1];
        t.adr = q[2];
        t.session = q[3];
        t.lba = (int32_t)get32(q + 4);
        t.stored = q[8];
        l.toc.push_back(t);
    }

    // ---- tracks
    uint32_t expect_index = 0;
    for (uint32_t i = 0; i < tc; i++) {
        auto &t = l.tracks[i];
        if (t.number != l.first_track + i) bad("track numbers not consecutive");
        if (t.index_count < 1 || t.first_index != expect_index) bad("track index range invalid");
        expect_index += t.index_count;
        if (t.length == 0 || t.start > l.total || t.length > l.total - t.start) bad("track out of timeline");
        if (i + 1 < tc && t.start + t.length != l.tracks[i + 1].start) bad("tracks not contiguous");
        if (i + 1 == tc && t.start + t.length != l.total) bad("last track does not end the timeline");
        if (!t.isrc.empty() && t.isrc.size() != 12) bad("invalid ISRC");
    }
    if (expect_index != ic) bad("index records not fully owned by tracks");

    // ---- indexes
    uint64_t lastpos = 0;
    for (uint32_t i = 0; i < tc; i++) {
        auto &t = l.tracks[i];
        int previdx = -1;
        bool has01 = false;
        uint64_t prev_start = i == 0 ? 0 : l.tracks[i - 1].start;
        for (uint32_t k = 0; k < t.index_count; k++) {
            auto &x = l.indexes[t.first_index + k];
            if (x.track != t.number) bad("index record belongs to wrong track");
            if ((int)x.index <= previdx || x.index > 99) bad("index numbers not increasing");
            previdx = x.index;
            if (x.pos < lastpos) bad("index positions decreasing");
            lastpos = x.pos;
            if (x.index == 0) {
                if (x.pos >= t.start || x.pos < prev_start) bad("INDEX 00 out of range");
            } else if (x.index == 1) {
                has01 = true;
                if (x.pos != t.start) bad("INDEX 01 does not match track start");
            } else if (x.pos < t.start || x.pos >= t.start + t.length) {
                bad("INDEX >= 02 outside its track");
            }
        }
        if (!has01) bad("track without INDEX 01");
    }

    // ---- segments
    uint64_t pos = 0;
    size_t ti = 0;
    std::set<uint32_t> chunk_refs;
    for (uint32_t i = 0; i < sc; i++) {
        auto &s = l.segments[i];
        if (s.start != pos || s.length == 0 || s.length > l.total - s.start) bad("segments not contiguous");
        pos += s.length;
        if (!chunk_refs.insert(s.chunk_index).second) bad("FLAC chunk referenced twice");
        if (s.kind == 0) {
            if (i != 0 || s.track != 0 || l.tracks[0].start != s.length) bad("invalid HTOA segment");
        } else if (s.kind == 1) {
            if (ti >= tc) bad("too many track segments");
            auto &t = l.tracks[ti++];
            if (s.track != t.number || s.start != t.start || s.length != t.length) bad("segment does not match its track");
        } else {
            bad("unknown segment kind");
        }
    }
    if (pos != l.total || ti != tc) bad("segments do not cover the timeline");
    bool htoa = l.tracks[0].start > 0;
    if (htoa != (sc == tc + 1)) bad("HTOA segment missing or unexpected");
    if (htoa != bool(l.flags & LF_HTOA)) bad("HTOA flag inconsistent");
    if ((l.flags & LF_CDDA) && !computeCddaFlag(l)) bad("CDDA flag set but layout is not CD-DA conformant");
    return l;
}

static std::string mbBase64(const std::string &raw) {
    static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    std::string o;
    size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        uint32_t v = (uint8_t)raw[i] << 16 | (uint8_t)raw[i + 1] << 8 | (uint8_t)raw[i + 2];
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += a[(v >> 6) & 63];
        o += a[v & 63];
    }
    if (raw.size() - i == 2) {
        uint32_t v = (uint8_t)raw[i] << 16 | (uint8_t)raw[i + 1] << 8;
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += a[(v >> 6) & 63];
        o += '-';
    } else if (raw.size() - i == 1) {
        uint32_t v = (uint8_t)raw[i] << 16;
        o += a[v >> 18];
        o += a[(v >> 12) & 63];
        o += "--";
    }
    return o;
}

std::string musicbrainzDiscId(const Layout &l, std::string *toc_string) {
    if (!l.cdda()) return "";
    int first = 0, last = 0;
    int32_t lead = 0;
    int32_t off[100] = {0};
    if ((l.flags & LF_HAS_TOC) && !l.toc.empty()) {
        int32_t data_after = -1;
        for (auto &t : l.toc) {
            if (t.track == 0xAA) {
                if (t.session == 1 || lead == 0) lead = t.lba;
                continue;
            }
            bool audio = !(t.control & 4);
            if (audio && t.session == 1) {
                if (!first) first = t.track;
                last = t.track;
                off[t.track] = t.lba + 150;
            } else if (!audio && first && data_after < 0) {
                data_after = t.lba;
            }
        }
        if (data_after >= 0) lead = data_after - 11400;  // CD-Extra rule used by libdiscid
        if (!first || lead <= 0) return "";
    } else {
        first = l.first_track;
        last = l.last_track;
        for (auto &t : l.tracks) off[t.number] = l.start_lba + (int32_t)(t.start / kSectorSamples) + 150;
        lead = l.start_lba + (int32_t)(l.total / kSectorSamples);
    }
    off[0] = lead + 150;
    char buf[16];
    std::string s;
    snprintf(buf, sizeof buf, "%02X", first);
    s += buf;
    snprintf(buf, sizeof buf, "%02X", last);
    s += buf;
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof buf, "%08X", (unsigned)off[i]);
        s += buf;
    }
    if (toc_string) {
        std::string t = std::to_string(first) + " " + std::to_string(last) + " " + std::to_string(off[0]);
        for (int i = first; i <= last; i++) t += " " + std::to_string(off[i]);
        *toc_string = t;
    }
    return mbBase64(sha1_raw(s));
}

// ------------------------------------------------------------------ pictures
const char *pictureTypeName(uint32_t t) {
    switch (t) {
        case 0:
            return "other";
        case 1:
            return "file_icon";
        case 3:
            return "front_cover";
        case 4:
            return "back_cover";
        case 5:
            return "leaflet";
        case 6:
            return "media";
        case 8:
            return "artist";
        default:
            return "other";
    }
}

std::vector<uint8_t> serializePicture(const Picture &p) {
    std::vector<uint8_t> v;
    put32(v, p.type);
    put32(v, (uint32_t)p.mime.size());
    v.insert(v.end(), p.mime.begin(), p.mime.end());
    put32(v, (uint32_t)p.desc.size());
    v.insert(v.end(), p.desc.begin(), p.desc.end());
    put32(v, p.width);
    put32(v, p.height);
    put32(v, p.depth);
    put32(v, p.colors);
    put32(v, (uint32_t)p.data.size());
    v.insert(v.end(), p.data.begin(), p.data.end());
    return v;
}

Picture parsePicture(const uint8_t *p, size_t n) {
    Picture r;
    size_t o = 0;
    auto need = [&](size_t k) {
        if (k > n - o) fail(FISO_E_FORMAT, "PICT: truncated");
    };
    need(8);
    r.type = get32(p);
    uint32_t ml = get32(p + 4);
    o = 8;
    if (ml > 256) fail(FISO_E_FORMAT, "PICT: MIME too long");
    need(ml);
    r.mime.assign((const char *)p + o, ml);
    o += ml;
    need(4);
    uint32_t dl = get32(p + o);
    o += 4;
    if (dl > 65536) fail(FISO_E_FORMAT, "PICT: description too long");
    need(dl);
    r.desc.assign((const char *)p + o, dl);
    o += dl;
    need(20);
    r.width = get32(p + o);
    r.height = get32(p + o + 4);
    r.depth = get32(p + o + 8);
    r.colors = get32(p + o + 12);
    uint32_t len = get32(p + o + 16);
    o += 20;
    need(len);
    r.data.assign(p + o, p + o + len);
    o += len;
    if (o != n) fail(FISO_E_FORMAT, "PICT: length mismatch");
    return r;
}

static bool sniffImage(const std::vector<uint8_t> &d, std::string &mime, uint32_t &w, uint32_t &h, uint32_t &depth) {
    if (d.size() >= 24 && memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(d.data() + 12, "IHDR", 4) == 0) {
        mime = "image/png";
        w = (uint32_t)d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19];
        h = (uint32_t)d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23];
        uint32_t bitd = d[24], ct = d[25];
        uint32_t ch = ct == 2 ? 3 : ct == 6 ? 4 : ct == 4 ? 2 : 1;
        depth = bitd * ch;
        return true;
    }
    if (d.size() >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        mime = "image/jpeg";
        size_t i = 2;
        while (i + 9 < d.size()) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            uint32_t len = d[i + 2] << 8 | d[i + 3];
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = d[i + 5] << 8 | d[i + 6];
                w = d[i + 7] << 8 | d[i + 8];
                depth = d[i + 4] * d[i + 9];
                return true;
            }
            i += 2 + len;
        }
        return true;  // valid JPEG start, size unknown
    }
    return false;
}

Picture pictureFromFile(const std::string &path, uint32_t type) {
    Picture p;
    p.type = type;
    p.data = readWholeFile(path, kMaxPict - 1024);
    if (!sniffImage(p.data, p.mime, p.width, p.height, p.depth))
        fail(FISO_E_UNSUPPORTED, "'" + path + "' is not a JPEG or PNG image");
    return p;
}

// ------------------------------------------------------------------ META
json normalizeMeta(const json &in, const Layout &l, bool fill_provenance) {
    json m = in.is_object() ? in : json::object();
    m["schema"] = "fiso.meta/1";
    if (!m.contains("album") || !m["album"].is_object()) m["album"] = json::object();
    if (!m["album"].contains("title") || !m["album"]["title"].is_string() || m["album"]["title"].get<std::string>().empty())
        m["album"]["title"] = "Untitled";
    if (!m["album"].contains("group_id")) m["album"]["group_id"] = uuid4();
    if (!m.contains("disc") || !m["disc"].is_object()) m["disc"] = json::object();
    if (!m["disc"].contains("number")) m["disc"]["number"] = 1;
    if (!m["disc"].contains("total")) m["disc"]["total"] = m["disc"]["number"];
    if (m.contains("tracks") && m["tracks"].is_array())
        for (auto &x : m["tracks"]) {
            if (!x.is_object() || !x.contains("number") || !x["number"].is_number_integer())
                fail(FISO_E_FORMAT, "META: track entry without a number");
            int n = x["number"];
            bool known = false;
            for (auto &t : l.tracks)
                if (t.number == n) known = true;
            if (!known) fail(FISO_E_FORMAT, "META: track " + std::to_string(n) + " does not exist on this disc");
        }
    json tracks = json::array();
    for (auto &t : l.tracks) {
        json tj = {{"number", t.number}};
        if (m.contains("tracks") && m["tracks"].is_array())
            for (auto &x : m["tracks"])
                if (x.is_object() && x.value("number", -1) == t.number) {
                    tj = x;
                    tj["number"] = t.number;
                }
        tracks.push_back(tj);
    }
    m["tracks"] = tracks;
    if (fill_provenance) {
        if (!m.contains("provenance") || !m["provenance"].is_object()) m["provenance"] = json::object();
        m["provenance"]["creator"] = std::string("libfiso ") + FISO_LIB_VERSION;
        if (!m["provenance"].contains("created")) m["provenance"]["created"] = nowIso8601();
    }
    return m;
}

void validateMeta(const json &m, const Layout &l) {
    auto bad = [](const std::string &s) { fail(FISO_E_FORMAT, "META: " + s); };
    if (!m.is_object()) bad("not a JSON object");
    if (m.value("schema", "") != "fiso.meta/1") bad("schema must be \"fiso.meta/1\"");
    if (!m.contains("album") || !m["album"].is_object()) bad("missing album");
    if (!m["album"].contains("title") || !m["album"]["title"].is_string()) bad("missing album.title");
    for (auto &[k, v] : m["album"].items())
        if (k != "musicbrainz" && !v.is_string()) bad("album." + k + " must be a string");
    if (!m.contains("disc") || !m["disc"].is_object()) bad("missing disc");
    auto &d = m["disc"];
    if (!d.contains("number") || !d["number"].is_number_integer() || !d.contains("total") || !d["total"].is_number_integer())
        bad("disc.number and disc.total must be integers");
    int n = d["number"], t = d["total"];
    if (n < 1 || t < 1 || n > t || t > 999) bad("disc numbering invalid (1 <= number <= total <= 999)");
    if (!m.contains("tracks") || !m["tracks"].is_array()) bad("missing tracks");
    std::set<int> want, got;
    for (auto &tr : l.tracks) want.insert(tr.number);
    for (auto &x : m["tracks"]) {
        if (!x.is_object() || !x.contains("number") || !x["number"].is_number_integer()) bad("track entry without number");
        if (!got.insert(x["number"].get<int>()).second) bad("duplicate track number");
        for (const char *k : {"title", "artist"})
            if (x.contains(k) && !x[k].is_string()) bad(std::string("track ") + k + " must be a string");
    }
    if (want != got) bad("tracks do not match the disc layout");
}

}  // namespace fiso
