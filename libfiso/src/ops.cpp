// Info, verification, editing, extraction, grouping — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace fiso {

// ------------------------------------------------------------------ helpers
static std::string mstr(const json &o, const char *k) {
    return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
}
static json trackMeta(const json &meta, int number) {
    if (meta.contains("tracks"))
        for (auto &t : meta["tracks"])
            if (t.value("number", -1) == number) return t;
    return json::object();
}

json infoJson(Image &img) {
    const Layout &L = img.layout;
    json j;
    j["format_version"] = std::to_string(img.vmajor) + "." + std::to_string(img.vminor);
    j["file"] = img.path;
    j["audio"] = {{"codec", "FLAC"},          {"sample_rate", L.rate},    {"channels", L.channels},
                  {"bits_per_sample", L.bps}, {"total_samples", L.total}, {"duration_seconds", (double)L.total / L.rate}};
    j["cdda"] = L.cdda();
    j["htoa"] = bool(L.flags & LF_HTOA);
    j["physical_source"] = bool(L.flags & LF_PHYSICAL);
    j["synthesized_silence"] = bool(L.flags & LF_SYNTH_SILENCE);
    j["timeline_start_lba"] = L.start_lba;
    j["mcn"] = L.mcn;
    uint64_t pcm = L.pcmBytes();
    j["sizes"] = {{"file_bytes", img.file_size}, {"pcm_bytes", pcm}, {"ratio", pcm ? (double)img.file_size / pcm : 0.0}};
    std::string toc;
    std::string id = musicbrainzDiscId(L, &toc);
    if (!id.empty()) {
        j["musicbrainz_disc_id"] = id;
        j["musicbrainz_toc"] = toc;
    }
    j["meta"] = img.meta;
    json tracks = json::array();
    for (auto &t : L.tracks) {
        json tm = trackMeta(img.meta, t.number);
        json ix = json::array();
        uint64_t pregap = 0;
        for (int k = 0; k < t.index_count; k++) {
            auto &x = L.indexes[t.first_index + k];
            ix.push_back({x.index, x.pos});
            if (x.index == 0) pregap = t.start - x.pos;
        }
        uint64_t cbytes = 0;
        if (auto *s = img.segmentForTrack(t.number)) cbytes = img.chunks[s->chunk_index].length;
        tracks.push_back({{"number", t.number},
                          {"title", mstr(tm, "title")},
                          {"artist", mstr(tm, "artist")},
                          {"start", t.start},
                          {"length", t.length},
                          {"duration_seconds", (double)t.length / L.rate},
                          {"pregap_samples", pregap},
                          {"isrc", t.isrc},
                          {"pre_emphasis", bool(t.flags & 1)},
                          {"copy_permitted", bool(t.flags & 2)},
                          {"four_channel", bool(t.flags & 4)},
                          {"indexes", ix},
                          {"compressed_bytes", cbytes}});
    }
    j["tracks"] = tracks;
    if (L.flags & LF_HTOA) j["htoa_samples"] = L.tracks[0].start;
    json pics = json::array();
    for (size_t ci : img.pict_chunks) {
        try {
            Picture p = img.readPicture(ci);
            pics.push_back({{"chunk", ci},
                            {"type", p.type},
                            {"type_name", pictureTypeName(p.type)},
                            {"mime", p.mime},
                            {"width", p.width},
                            {"height", p.height},
                            {"bytes", p.data.size()},
                            {"description", p.desc}});
        } catch (const Error &e) {
            img.warnings.push_back(std::string("picture unreadable: ") + e.what());
        }
    }
    j["pictures"] = pics;
    json toc_j = json::array();
    for (auto &t : L.toc)
        toc_j.push_back(
            {{"track", t.track}, {"control", t.control}, {"session", t.session}, {"lba", t.lba}, {"stored", (bool)t.stored}});
    j["toc"] = toc_j;
    json ch = json::array();
    for (auto &c : img.chunks)
        ch.push_back({{"type", c.type}, {"offset", c.offset}, {"length", c.length}, {"critical", bool(c.flags & 1)}});
    j["chunks"] = ch;
    j["warnings"] = img.warnings;
    return j;
}

// ------------------------------------------------------------------ verify
json verifyImage(const std::string &path, bool deep, const Progress &prog) {
    json rep;
    rep["file"] = path;
    json vf = {{"ok", false}, {"errors", json::array()}, {"warnings", json::array()}};
    json ia = {{"ok", nullptr}, {"checked", false}, {"errors", json::array()}, {"segments", json::array()}};
    json vr = {{"state", "not_verified"}, {"method", ""}};
    std::unique_ptr<Image> img;
    try {
        img = Image::open(path);
    } catch (const Error &e) {
        vf["errors"].push_back(e.what());
        rep["valid_file"] = vf;
        rep["intact_audio"] = ia;
        rep["verified_rip"] = vr;
        return rep;
    }
    rep["format_version"] = std::to_string(img->vmajor) + "." + std::to_string(img->vminor);
    for (auto &w : img->warnings) vf["warnings"].push_back(w);
    uint64_t work = 0, done = 0;
    for (auto &c : img->chunks) work += c.length;
    if (deep) work += img->layout.total;
    // every chunk hash
    std::vector<uint8_t> buf(1 << 20);
    for (size_t i = 0; i < img->chunks.size(); i++) {
        auto &c = img->chunks[i];
        Sha256 h;
        uint64_t left = c.length, off = 0;
        while (left) {
            size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            img->file().seek(c.offset + off);
            img->file().read(buf.data(), n);
            h.update(buf.data(), n);
            left -= n;
            off += n;
            done += n;
            prog("verify", done, work);
        }
        if (h.finish() != c.sha)
            vf["errors"].push_back("chunk " + std::to_string(i) + " (" + c.type + "): SHA-256 mismatch");
        else if (c.type == "PICT") {
            try {
                img->readPicture(i);
            } catch (const Error &e) {
                vf["errors"].push_back(std::string("PICT: ") + e.what());
            }
        }
    }
    vf["ok"] = vf["errors"].empty();

    if (deep) {
        ia["checked"] = true;
        Sha256 tl;
        bool all_ok = true;
        const Layout &L = img->layout;
        std::vector<int32_t> pcm(16384 * L.channels);
        std::vector<uint8_t> bytes;
        for (auto &s : L.segments) {
            json sj = {{"kind", s.kind == 0 ? "htoa" : "track"}, {"track", s.track}};
            auto &c = img->chunks[s.chunk_index];
            try {
                SegmentDecoder d(img->file(), c.offset, c.length);
                if (d.rate() != L.rate || d.channels() != L.channels || d.bps() != L.bps)
                    fail(FISO_E_FORMAT, "STREAMINFO format differs from LAYT");
                if (d.totalSamples() != s.length) fail(FISO_E_FORMAT, "STREAMINFO length differs from LAYT");
                static const uint8_t z[16] = {0};
                bool md5_present = memcmp(d.md5(), z, 16) != 0;
                Sha256 sh;
                uint64_t left = s.length;
                while (left) {
                    size_t n = (size_t)std::min<uint64_t>(left, 16384);
                    if (d.read(pcm.data(), n) != n) fail(FISO_E_INTEGRITY, "stream shorter than declared");
                    bytes.clear();
                    canonicalBytes(pcm.data(), n * L.channels, L.bps, bytes);
                    sh.update(bytes.data(), bytes.size());
                    tl.update(bytes.data(), bytes.size());
                    left -= n;
                    done += n;
                    prog("verify", done, work);
                }
                int32_t extra;
                if (d.read(&extra, 1) != 0) fail(FISO_E_INTEGRITY, "stream longer than declared");
                bool md5 = d.md5Ok() && md5_present;
                bool sha = sh.finish() == s.sha;
                sj["flac_md5_ok"] = md5;
                sj["pcm_sha256_ok"] = sha;
                if (!md5) ia["errors"].push_back("segment " + std::to_string(s.track) + ": FLAC MD5 missing or mismatch");
                if (!sha) ia["errors"].push_back("segment " + std::to_string(s.track) + ": PCM SHA-256 mismatch");
                all_ok = all_ok && md5 && sha;
            } catch (const Error &e) {
                if (e.code == FISO_E_CANCELLED) throw;
                sj["error"] = e.what();
                ia["errors"].push_back("segment " + std::to_string(s.track) + ": " + e.what());
                all_ok = false;
                // keep the timeline hash meaningful: it can no longer match
            }
            ia["segments"].push_back(sj);
        }
        bool tok = all_ok && tl.finish() == L.timeline_sha;
        ia["timeline_sha256_ok"] = tok;
        if (all_ok && !tok) ia["errors"].push_back("timeline PCM SHA-256 mismatch");
        ia["ok"] = all_ok && tok;
    }
    const json &prov = img->meta.contains("provenance") ? img->meta["provenance"] : json::object();
    if (prov.contains("rip") && prov["rip"].value("verified", false) && !mstr(prov["rip"], "verification").empty()) {
        vr["state"] = "verified";
        vr["method"] = prov["rip"]["verification"];
    }
    rep["valid_file"] = vf;
    rep["intact_audio"] = ia;
    rep["verified_rip"] = vr;
    return rep;
}

// ------------------------------------------------------------------ edit
void editImage(const std::string &path, const json &edit, const std::string &out_path, const Progress &prog) {
    auto img = Image::open(path);
    json meta = edit.contains("meta") ? edit["meta"] : img->meta;
    meta = normalizeMeta(meta, img->layout, false);
    if (img->meta.contains("provenance") && !meta.contains("provenance")) meta["provenance"] = img->meta["provenance"];
    validateMeta(meta, img->layout);

    std::set<uint32_t> remove_types;
    bool remove_all = edit.value("remove_all_pictures", false);
    if (edit.contains("remove_picture_types"))
        for (auto &t : edit["remove_picture_types"]) remove_types.insert(t.get<uint32_t>());
    std::vector<Picture> add;
    if (edit.contains("pictures"))
        for (auto &p : edit["pictures"]) {
            add.push_back(pictureFromFile(p.at("path").get<std::string>(), p.value("type", 3u)));
            add.back().desc = p.value("description", std::string());
            remove_types.insert(add.back().type);  // a new picture replaces the old one of the same type
        }

    std::string target = out_path.empty() ? path : out_path;
    bool overwrite = out_path.empty() ? true : edit.value("overwrite", false);
    ContainerWriter w(target, overwrite);
    std::map<uint32_t, uint32_t> remap;
    for (size_t i = 0; i < img->chunks.size(); i++) {
        auto &c = img->chunks[i];
        if (c.type == "LAYT" || c.type == "META" || c.type == "PICT") continue;
        remap[(uint32_t)i] = (uint32_t)w.copyChunk(*img, i, prog);
    }
    Layout L = img->layout;
    for (auto &s : L.segments) s.chunk_index = remap.at(s.chunk_index);
    w.addChunk("LAYT", kChunkCritical, serializeLayout(L));
    std::string md = meta.dump(2);
    w.addChunk("META", kChunkCritical, std::vector<uint8_t>(md.begin(), md.end()));
    for (size_t ci : img->pict_chunks) {
        Picture p = img->readPicture(ci);
        if (remove_all || remove_types.count(p.type)) continue;
        w.addChunk("PICT", 0, serializePicture(p));
    }
    for (auto &p : add) w.addChunk("PICT", 0, serializePicture(p));
    img.reset();  // release the source before replacing it (required on Windows)
    w.finish();
}

// ------------------------------------------------------------------ FLAC export with tags
static void vcAdd(std::vector<uint8_t> &v, const std::string &k, const std::string &val, uint32_t &count) {
    if (val.empty()) return;
    std::string e = k + "=" + val;
    put32(v, (uint32_t)e.size());
    v.insert(v.end(), e.begin(), e.end());
    count++;
}
static void be32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 3; i >= 0; i--) v.push_back((x >> (8 * i)) & 0xff);
}

std::vector<uint8_t> flacWithTags(Image &img, const SegmentRec &seg, bool with_picture) {
    auto &c = img.chunks[seg.chunk_index];
    // read metadata region of the stored stream
    File &f = img.file();
    uint8_t mk[4];
    f.seek(c.offset);
    f.read(mk, 4);
    if (memcmp(mk, "fLaC", 4) != 0) fail(FISO_E_FORMAT, "FLAC segment without fLaC marker");
    std::vector<std::pair<int, std::vector<uint8_t>>> keep;
    uint64_t pos = 4;
    bool last = false;
    while (!last) {
        uint8_t bh[4];
        if (pos + 4 > c.length) fail(FISO_E_FORMAT, "truncated FLAC metadata");
        f.seek(c.offset + pos);
        f.read(bh, 4);
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t len = (uint32_t)bh[1] << 16 | bh[2] << 8 | bh[3];
        if (pos + 4 + len > c.length) fail(FISO_E_FORMAT, "truncated FLAC metadata block");
        if (type == 0 || type == 3) {
            std::vector<uint8_t> b(len);
            f.read(b.data(), len);
            keep.push_back({type, b});
        }
        pos += 4 + len;
    }
    const json &m = img.meta;
    json tm = seg.kind == 1 ? trackMeta(m, seg.track) : json::object();
    const json &al = m.contains("album") ? m["album"] : json::object();
    const json &mb = al.contains("musicbrainz") ? al["musicbrainz"] : json::object();
    const json &d = m["disc"];
    std::vector<uint8_t> vc;
    std::string vendor = std::string("libfiso ") + FISO_LIB_VERSION;
    put32(vc, (uint32_t)vendor.size());
    vc.insert(vc.end(), vendor.begin(), vendor.end());
    size_t count_at = vc.size();
    put32(vc, 0);
    uint32_t n = 0;
    std::string title = mstr(tm, "title");
    if (seg.kind == 0 && title.empty()) title = "Hidden track one audio";
    vcAdd(vc, "TITLE", title, n);
    std::string artist = mstr(tm, "artist");
    vcAdd(vc, "ARTIST", artist.empty() ? mstr(al, "artist") : artist, n);
    vcAdd(vc, "ALBUM", mstr(al, "title"), n);
    vcAdd(vc, "ALBUMARTIST", mstr(al, "artist"), n);
    if (seg.kind == 1) {
        vcAdd(vc, "TRACKNUMBER", std::to_string(seg.track), n);
        vcAdd(vc, "TRACKTOTAL", std::to_string(img.layout.tracks.size()), n);
    }
    vcAdd(vc, "DISCNUMBER", std::to_string(d.value("number", 1)), n);
    vcAdd(vc, "DISCTOTAL", std::to_string(d.value("total", 1)), n);
    vcAdd(vc, "DISCSUBTITLE", mstr(d, "subtitle"), n);
    vcAdd(vc, "DATE", mstr(al, "date"), n);
    vcAdd(vc, "GENRE", mstr(al, "genre"), n);
    vcAdd(vc, "LABEL", mstr(al, "label"), n);
    vcAdd(vc, "CATALOGNUMBER", mstr(al, "catalog_number"), n);
    vcAdd(vc, "BARCODE", mstr(al, "barcode"), n);
    vcAdd(vc, "RELEASECOUNTRY", mstr(al, "country"), n);
    vcAdd(vc, "ALBUM_GROUP_ID", mstr(al, "group_id"), n);
    vcAdd(vc, "MUSICBRAINZ_ALBUMID", mstr(mb, "release_id"), n);
    vcAdd(vc, "MUSICBRAINZ_RELEASEGROUPID", mstr(mb, "release_group_id"), n);
    vcAdd(vc, "MUSICBRAINZ_ALBUMARTISTID", mstr(mb, "album_artist_id"), n);
    vcAdd(vc, "MUSICBRAINZ_DISCID", mstr(mb, "disc_id"), n);
    if (tm.contains("musicbrainz")) {
        vcAdd(vc, "MUSICBRAINZ_TRACKID", mstr(tm["musicbrainz"], "recording_id"), n);
        vcAdd(vc, "MUSICBRAINZ_RELEASETRACKID", mstr(tm["musicbrainz"], "track_id"), n);
    }
    if (seg.kind == 1) {
        for (auto &t : img.layout.tracks)
            if (t.number == seg.track) vcAdd(vc, "ISRC", t.isrc, n);
    }
    if (m.contains("tags") && m["tags"].is_object())
        for (auto &[k, v] : m["tags"].items())
            if (v.is_string()) vcAdd(vc, k, v.get<std::string>(), n);
    vc[count_at] = n & 0xff;
    vc[count_at + 1] = (n >> 8) & 0xff;
    vc[count_at + 2] = (n >> 16) & 0xff;
    vc[count_at + 3] = n >> 24;

    std::vector<uint8_t> pic;
    if (with_picture) {
        for (size_t ci : img.pict_chunks) {
            Picture p = img.readPicture(ci);
            if (p.type != 3) continue;
            be32(pic, p.type);
            be32(pic, (uint32_t)p.mime.size());
            pic.insert(pic.end(), p.mime.begin(), p.mime.end());
            be32(pic, (uint32_t)p.desc.size());
            pic.insert(pic.end(), p.desc.begin(), p.desc.end());
            be32(pic, p.width);
            be32(pic, p.height);
            be32(pic, p.depth);
            be32(pic, p.colors);
            be32(pic, (uint32_t)p.data.size());
            pic.insert(pic.end(), p.data.begin(), p.data.end());
            if (pic.size() >= (1u << 24)) pic.clear();  // does not fit a FLAC block
            break;
        }
    }
    std::vector<std::pair<int, std::vector<uint8_t>>> blocks = keep;
    blocks.push_back({4, vc});
    if (!pic.empty()) blocks.push_back({6, pic});
    blocks.push_back({1, std::vector<uint8_t>(8192, 0)});  // padding for later tag edits
    std::vector<uint8_t> out = {'f', 'L', 'a', 'C'};
    for (size_t i = 0; i < blocks.size(); i++) {
        auto &[type, b] = blocks[i];
        out.push_back((uint8_t)((i + 1 == blocks.size() ? 0x80 : 0) | type));
        out.push_back((uint8_t)(b.size() >> 16));
        out.push_back((uint8_t)(b.size() >> 8));
        out.push_back((uint8_t)b.size());
        out.insert(out.end(), b.begin(), b.end());
    }
    // append the audio frames byte-for-byte
    size_t head = out.size();
    out.resize(head + (size_t)(c.length - pos));
    f.seek(c.offset + pos);
    f.read(out.data() + head, (size_t)(c.length - pos));
    return out;
}

// ------------------------------------------------------------------ CUE
static std::string q(const std::string &s) {
    std::string r = s;
    std::replace(r.begin(), r.end(), '"', '\'');
    return "\"" + r + "\"";
}

std::string buildCue(Image &img, const std::string &bin_name) {
    const Layout &L = img.layout;
    const json &m = img.meta;
    const json &al = m.contains("album") ? m["album"] : json::object();
    std::string c;
    if (!mstr(al, "genre").empty()) c += "REM GENRE " + q(mstr(al, "genre")) + "\n";
    if (!mstr(al, "date").empty()) c += "REM DATE " + mstr(al, "date").substr(0, 10) + "\n";
    c += "REM DISCNUMBER " + std::to_string(m["disc"].value("number", 1)) + "\n";
    c += "REM TOTALDISCS " + std::to_string(m["disc"].value("total", 1)) + "\n";
    c += std::string("REM COMMENT \"libfiso ") + FISO_LIB_VERSION + "\"\n";
    if (!L.mcn.empty()) c += "CATALOG " + L.mcn + "\n";
    if (!mstr(al, "artist").empty()) c += "PERFORMER " + q(mstr(al, "artist")) + "\n";
    c += "TITLE " + q(mstr(al, "title")) + "\n";
    c += "FILE " + q(bin_name) + " BINARY\n";
    for (auto &t : L.tracks) {
        json tm = trackMeta(m, t.number);
        char buf[64];
        snprintf(buf, sizeof buf, "  TRACK %02d AUDIO\n", t.number);
        c += buf;
        if (!mstr(tm, "title").empty()) c += "    TITLE " + q(mstr(tm, "title")) + "\n";
        if (!mstr(tm, "artist").empty()) c += "    PERFORMER " + q(mstr(tm, "artist")) + "\n";
        std::string fl;
        if (t.flags & 2) fl += " DCP";
        if (t.flags & 4) fl += " 4CH";
        if (t.flags & 1) fl += " PRE";
        if (!fl.empty()) c += "    FLAGS" + fl + "\n";
        if (!t.isrc.empty()) c += "    ISRC " + t.isrc + "\n";
        for (int k = 0; k < t.index_count; k++) {
            auto &x = L.indexes[t.first_index + k];
            snprintf(buf, sizeof buf, "    INDEX %02d ", x.index);
            c += buf + msf(x.pos / kSectorSamples) + "\n";
        }
    }
    return c;
}

// ------------------------------------------------------------------ extract
static std::string render(const std::string &tpl, const std::map<std::string, std::string> &v) {
    std::string out;
    for (size_t i = 0; i < tpl.size();) {
        if (tpl[i] == '{') {
            size_t e = tpl.find('}', i);
            if (e != std::string::npos) {
                std::string key = tpl.substr(i + 1, e - i - 1), pad;
                auto colon = key.find(':');
                if (colon != std::string::npos) {
                    pad = key.substr(colon + 1);
                    key = key.substr(0, colon);
                }
                auto it = v.find(key);
                if (it != v.end()) {
                    std::string s = it->second;
                    if (!pad.empty() && pad[0] == '0') {
                        size_t w = (size_t)atoi(pad.c_str());
                        while (s.size() < w) s = "0" + s;
                    }
                    out += s;
                    i = e + 1;
                    continue;
                }
            }
        }
        out.push_back(tpl[i++]);
    }
    return out;
}

static void writeAtomic(const std::string &path, const std::function<void(File &)> &fn, bool overwrite) {
    if (!overwrite && pathExists(path)) fail(FISO_E_EXISTS, "'" + path + "' already exists");
    std::string tmp = tempPathFor(path);
    try {
        File f = File::open(tmp, "wb");
        fn(f);
        f.sync();
        f.close();
        renameReplace(tmp, path, overwrite);
    } catch (...) {
        removeFile(tmp);
        throw;
    }
}

static void wavHeader(File &f, const Layout &L, uint64_t frames) {
    uint32_t bytes = L.bytesPerSample();
    uint64_t data = frames * L.channels * bytes;
    if (data > 0xFFFFFFFFull - 80) fail(FISO_E_UNSUPPORTED, "WAV output larger than 4 GiB");
    bool ext = L.channels > 2 || L.bps > 16 || L.bps % 8;
    std::vector<uint8_t> h = {'R', 'I', 'F', 'F'};
    put32(h, (uint32_t)(4 + 8 + (ext ? 40 : 16) + 8 + data));
    for (char ch : std::string("WAVEfmt ")) h.push_back((uint8_t)ch);
    put32(h, ext ? 40 : 16);
    put16(h, ext ? 0xFFFE : 1);
    put16(h, L.channels);
    put32(h, L.rate);
    put32(h, L.rate * L.channels * bytes);
    put16(h, (uint16_t)(L.channels * bytes));
    put16(h, (uint16_t)(bytes * 8));
    if (ext) {
        put16(h, 22);
        put16(h, L.bps);
        put32(h, L.channels == 2 ? 3 : L.channels == 1 ? 4 : 0);
        static const uint8_t guid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        h.insert(h.end(), guid, guid + 16);
    }
    for (char ch : std::string("data")) h.push_back((uint8_t)ch);
    put32(h, (uint32_t)data);
    f.write(h.data(), h.size());
}

static void writePcmRange(Image &img, File &f, uint64_t start, uint64_t len, const Progress &prog, uint64_t &done, uint64_t all) {
    const Layout &L = img.layout;
    std::vector<int32_t> pcm(16384 * L.channels);
    std::vector<uint8_t> bytes;
    uint32_t bytes_ps = L.bytesPerSample();
    uint32_t shift = bytes_ps * 8 - L.bps;  // WAV/BIN containers are left-justified
    while (len) {
        size_t n = (size_t)std::min<uint64_t>(len, 16384);
        if (img.readPcm(start, n, pcm.data()) != n) fail(FISO_E_INTEGRITY, "decoded audio shorter than expected");
        if (shift)
            for (size_t i = 0; i < n * L.channels; i++) pcm[i] = (int32_t)((uint32_t)pcm[i] << shift);
        bytes.clear();
        canonicalBytes(pcm.data(), n * L.channels, bytes_ps * 8, bytes);
        f.write(bytes.data(), bytes.size());
        start += n;
        len -= n;
        done += n;
        prog("extract", done, all);
    }
}

// Progress of one phase of a two-phase operation, mapped to [lo, hi] of the whole.
struct PhaseCtx {
    const Progress *p;
    double lo, hi;
    const char *rename = nullptr;
};
static int phaseCb(void *u, const char *phase, uint64_t d, uint64_t t) {
    auto *c = (PhaseCtx *)u;
    double f = t ? std::min(1.0, (double)d / (double)t) : 0.0;
    if (c->rename && std::strcmp(phase, "convert") != 0) phase = c->rename;
    (*c->p)(phase, (uint64_t)((c->lo + f * (c->hi - c->lo)) * 1000000.0), 1000000);
    return 0;
}

// "Artist - Album (Disc n)": base name of the files written for a whole disc.
static std::string discFileStem(const json &m) {
    const json &al = m["album"];
    int disc = m["disc"].value("number", 1), disc_total = m["disc"].value("total", 1);
    std::string album = mstr(al, "title").empty() ? "Album" : mstr(al, "title");
    std::string artist = mstr(al, "artist");
    return sanitizeFilename((artist.empty() ? "" : artist + " - ") + album +
                            (disc_total > 1 ? " (Disc " + std::to_string(disc) + ")" : ""));
}

static json extractFromImage(const std::string &path, const json &opts, const Progress &prog, bool reportSynthesizedSilence);

// "bin-cue": a BIN/CUE image that any burning software can write as an audio CD.
// A CD-DA image is written as it is; otherwise a CD version is derived first (track starts
// on CD sectors, conversion only when "allow_conversion" is true), then removed.
// The FISO image itself is never modified.
static json extractCdBinCue(const std::string &path, const json &opts, const Progress &prog) {
    auto src = Image::open(path);
    std::string dir = opts.value("out_dir", ".");
    std::string stem = discFileStem(src->meta);
    if (!opts.value("overwrite", false))
        for (const char *ext : {".bin", ".cue"})
            if (pathExists(joinPath(dir, stem + ext))) fail(FISO_E_EXISTS, "'" + joinPath(dir, stem + ext) + "' already exists");
    bool synthesized = src->layout.flags & LF_SYNTH_SILENCE;
    if (src->layout.cdda()) {
        src.reset();
        json r = extractFromImage(path, opts, prog, true);
        r["cd_conversion"] = false;
        return r;
    }
    src.reset();
    makeDirs(dir);
    std::string tmp = tempPathFor(joinPath(dir, "cd-version.fiso"));
    struct RemoveOnExit {
        std::string p;
        ~RemoveOnExit() { removeFile(p); }
    } cleanup{tmp};
    PhaseCtx prepare{&prog, 0.0, 0.6, "cd_prepare"}, write{&prog, 0.6, 1.0, "cd_write"};
    json plan = {{"output", tmp},
                 {"overwrite", true},
                 {"compression", 0},
                 {"verify", true},
                 {"source", {{"kind", "fiso"}, {"path", path}}},
                 {"cd_compatible", true},
                 {"allow_conversion", opts.value("allow_conversion", false)}};
    json created = createImage(plan, Progress{phaseCb, &prepare});
    if (!created.value("cdda", false)) fail(FISO_E_INTERNAL, "the CD version is not CD-DA conformant");
    json mastering = Image::open(tmp)->meta["provenance"].value("cd_mastering", json::object());
    // The silence added at the end of the CD version is reported as [end_padding], not as CUE PREGAP silence.
    json r = extractFromImage(tmp, opts, Progress{phaseCb, &write}, synthesized);
    json warnings = created.value("warnings", json::array());
    for (auto &w : r["warnings"]) warnings.push_back(w);
    r["warnings"] = warnings;
    r["cd_conversion"] = true;
    r["cd_mastering"] = mastering;
    return r;
}

json extractImage(const std::string &path, const json &opts, const Progress &prog) {
    if (opts.value("format", "flac") == "bin-cue") return extractCdBinCue(path, opts, prog);
    return extractFromImage(path, opts, prog, true);
}

static json extractFromImage(const std::string &path, const json &opts, const Progress &prog, bool reportSynthesizedSilence) {
    auto img = Image::open(path);
    const Layout &L = img->layout;
    const json &m = img->meta;
    const json &al = m["album"];
    std::string fmt = opts.value("format", "flac");
    std::string dir = opts.value("out_dir", ".");
    bool overwrite = opts.value("overwrite", false);
    bool htoa = opts.value("include_htoa", false);
    int disc = m["disc"].value("number", 1), disc_total = m["disc"].value("total", 1);
    std::string tpl = opts.value("template", disc_total > 1 ? "Disc {disc} - {track:02} - {title}" : "{track:02} - {title}");
    makeDirs(dir);

    std::set<int> sel;
    if (opts.contains("tracks") && opts["tracks"].is_array() && !opts["tracks"].empty())
        for (auto &t : opts["tracks"]) sel.insert(t.get<int>());
    else
        for (auto &t : L.tracks) sel.insert(t.number);
    for (int t : sel)
        if (t < L.first_track || t > L.last_track) fail(FISO_E_ARG, "no track " + std::to_string(t) + " in this image");

    std::map<std::string, std::string> base = {
        {"album", mstr(al, "title")},
        {"albumartist", mstr(al, "artist")},
        {"artist", mstr(al, "artist")},
        {"disc", std::to_string(disc)},
        {"disctotal", std::to_string(disc_total)},
        {"year", mstr(al, "date").substr(0, std::min<size_t>(4, mstr(al, "date").size()))}};
    std::string albumName = base["album"].empty() ? "Album" : base["album"];
    std::string stem = discFileStem(m);

    json files = json::array(), warnings = json::array();
    uint64_t done = 0;

    if (fmt == "flac" || fmt == "wav") {
        std::vector<const SegmentRec *> segs;
        for (auto &s : L.segments)
            if ((s.kind == 1 && sel.count(s.track)) || (s.kind == 0 && htoa)) segs.push_back(&s);
        uint64_t all = 0;
        for (auto *s : segs) all += fmt == "wav" ? s->length : img->chunks[s->chunk_index].length;
        std::vector<std::string> names;
        for (auto *s : segs) {
            std::map<std::string, std::string> v = base;
            json tm = trackMeta(m, s->track);
            v["track"] = std::to_string(s->track);
            v["title"] = mstr(tm, "title").empty()
                             ? (s->kind == 0 ? "Hidden track one audio" : "Track " + std::to_string(s->track))
                             : mstr(tm, "title");
            if (!mstr(tm, "artist").empty()) v["artist"] = mstr(tm, "artist");
            std::string name = sanitizeFilename(render(tpl, v)) + "." + fmt;
            std::string p = joinPath(dir, name);
            if (!overwrite && pathExists(p)) fail(FISO_E_EXISTS, "'" + p + "' already exists");
            names.push_back(p);
        }
        for (size_t i = 0; i < segs.size(); i++) {
            auto *s = segs[i];
            if (fmt == "flac") {
                auto bytes = flacWithTags(*img, *s, true);
                writeAtomic(names[i], [&](File &f) { f.write(bytes.data(), bytes.size()); }, overwrite);
                done += img->chunks[s->chunk_index].length;
                prog("extract", done, all);
            } else {
                writeAtomic(
                    names[i],
                    [&](File &f) {
                        wavHeader(f, L, s->length);
                        writePcmRange(*img, f, s->start, s->length, prog, done, all);
                    },
                    overwrite);
            }
            files.push_back(names[i]);
        }
        json r = {{"files", files}, {"warnings", warnings}, {"reencoded", false}};
        bool has_pregaps = false;
        for (auto &x : L.indexes)
            if (x.index == 0 && x.track != L.first_track) has_pregaps = true;
        if (has_pregaps)
            r["warnings"].push_back("pre-gaps (INDEX 00) are stored at the end of the previous track, as on the disc timeline");
        return r;
    }
    if (fmt == "bin-cue") {
        std::string bin = joinPath(dir, stem + ".bin"), cue = joinPath(dir, stem + ".cue");
        for (auto &p : {bin, cue})
            if (!overwrite && pathExists(p)) fail(FISO_E_EXISTS, "'" + p + "' already exists");
        if (!L.cdda()) fail(FISO_E_INTERNAL, "BIN/CUE is written from a CD-DA timeline only");
        if (L.start_lba != 0)
            warnings.push_back("timeline does not start at LBA 0: the disc start offset cannot be expressed in a CUE sheet");
        if ((L.flags & LF_SYNTH_SILENCE) && reportSynthesizedSilence)
            warnings.push_back("some pre-gap silence was generated from a CUE PREGAP and is written as audio");
        writeAtomic(bin, [&](File &f) { writePcmRange(*img, f, 0, L.total, prog, done, L.total); }, overwrite);
        std::string c = buildCue(*img, baseName(bin));
        writeAtomic(cue, [&](File &f) { f.write(c.data(), c.size()); }, overwrite);
        return {{"files", {bin, cue}}, {"warnings", warnings}, {"reencoded", false}};
    }
    if (fmt == "cover") {
        for (size_t ci : img->pict_chunks) {
            Picture p = img->readPicture(ci);
            std::string ext = p.mime == "image/png" ? ".png" : p.mime == "image/jpeg" ? ".jpg" : ".bin";
            std::string name = joinPath(dir, sanitizeFilename(albumName + " - " + pictureTypeName(p.type)) + ext);
            for (int k = 2; std::find(files.begin(), files.end(), json(name)) != files.end(); k++)
                name =
                    joinPath(dir, sanitizeFilename(albumName + " - " + pictureTypeName(p.type) + " " + std::to_string(k)) + ext);
            writeAtomic(name, [&](File &f) { f.write(p.data.data(), p.data.size()); }, overwrite);
            files.push_back(name);
        }
        if (files.empty()) warnings.push_back("this image contains no pictures");
        return {{"files", files}, {"warnings", warnings}};
    }
    if (fmt == "meta") {
        std::string name = joinPath(dir, stem + ".json");
        std::string s = m.dump(2);
        writeAtomic(name, [&](File &f) { f.write(s.data(), s.size()); }, overwrite);
        return {{"files", {name}}, {"warnings", warnings}};
    }
    fail(FISO_E_ARG, "unknown extraction format '" + fmt + "' (flac, wav, bin-cue, cover, meta)");
}

// ------------------------------------------------------------------ group
json groupImages(const std::vector<std::string> &paths) {
    struct Disc {
        int number;
        std::string path, subtitle;
        size_t tracks;
    };
    struct G {
        std::string method, album, artist;
        int total = 0;
        std::vector<Disc> discs;
    };
    std::map<std::string, G> groups;
    json errors = json::array();
    for (auto &p : paths) {
        try {
            auto img = Image::open(p);
            const json &m = img->meta;
            const json &al = m["album"];
            std::string key, method;
            std::string rel = al.contains("musicbrainz") ? mstr(al["musicbrainz"], "release_id") : "";
            int total = m["disc"].value("total", 1);
            if (!rel.empty()) {
                key = "mb:" + rel;
                method = "musicbrainz_release";
            } else if (!mstr(al, "group_id").empty()) {
                key = "gid:" + mstr(al, "group_id");
                method = "group_id";
            } else {
                key = "guess:" + mstr(al, "title") + "\x1f" + mstr(al, "artist") + "\x1f" + std::to_string(total);
                method = "heuristic";
            }
            auto &g = groups[key];
            g.method = method;
            g.album = mstr(al, "title");
            g.artist = mstr(al, "artist");
            g.total = std::max(g.total, total);
            g.discs.push_back({m["disc"].value("number", 1), p, mstr(m["disc"], "subtitle"), img->layout.tracks.size()});
        } catch (const Error &e) {
            errors.push_back({{"path", p}, {"error", e.what()}});
        }
    }
    json out = json::array();
    for (auto &[k, g] : groups) {
        std::sort(g.discs.begin(), g.discs.end(), [](const Disc &a, const Disc &b) { return a.number < b.number; });
        json discs = json::array(), missing = json::array(), dup = json::array();
        std::set<int> seen;
        for (auto &d : g.discs) {
            discs.push_back({{"number", d.number}, {"path", d.path}, {"tracks", d.tracks}, {"subtitle", d.subtitle}});
            if (!seen.insert(d.number).second) dup.push_back(d.number);
        }
        for (int i = 1; i <= g.total; i++)
            if (!seen.count(i)) missing.push_back(i);
        out.push_back({{"album", g.album},
                       {"artist", g.artist},
                       {"disc_total", g.total},
                       {"method", g.method},
                       {"discs", discs},
                       {"missing_discs", missing},
                       {"duplicate_discs", dup}});
    }
    return {{"groups", out}, {"errors", errors}};
}

}  // namespace fiso
