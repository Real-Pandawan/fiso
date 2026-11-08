// fiso — command-line tool for FISO images. Uses only the public libfiso C API.
// SPDX-License-Identifier: Apache-2.0
#include <fiso/fiso.h>

#include "../libfiso/third_party/nlohmann/json.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#define ISATTY isatty
#define FILENO fileno
#endif

using json = nlohmann::json;

static std::atomic<int> g_cancel{0};
static bool g_progress_json = false;
static bool g_tty = false;

static void onSignal(int) {
    g_cancel = 1;
}

static int progressCb(void *, const char *phase, uint64_t done, uint64_t total) {
    static int last = -1;
    static std::string lastPhase;
    int pct = total ? (int)(done * 1000 / total) : 0;
    if (pct != last || lastPhase != phase) {
        last = pct;
        lastPhase = phase;
        if (g_progress_json)
            fprintf(stderr, "{\"progress\":{\"phase\":\"%s\",\"done\":%llu,\"total\":%llu}}\n", phase, (unsigned long long)done,
                    (unsigned long long)total);
        else if (g_tty)
            fprintf(stderr, "\r%-8s %5.1f %%", phase, pct / 10.0);
        fflush(stderr);
    }
    return g_cancel.load();
}

static void endProgress() {
    if (g_tty && !g_progress_json) fprintf(stderr, "\r%30s\r", "");
}

static int die(int st, const std::string &extra = "") {
    endProgress();
    std::string msg = extra.empty() ? fiso_last_error() : extra;
    if (g_progress_json)
        fprintf(stderr, "%s\n",
                json({{"error", {{"code", st}, {"status", fiso_status_string(st)}, {"message", msg}}}}).dump().c_str());
    else
        fprintf(stderr, "fiso: %s: %s\n", fiso_status_string(st), msg.c_str());
    return st;
}

static std::string take(char **p) {
    std::string s = *p ? *p : "";
    fiso_free(*p);
    *p = nullptr;
    return s;
}

// All paths are UTF-8 strings; std::filesystem converts them for the platform (UTF-16 on Windows).
static std::filesystem::path u8path(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

static FILE *openFile(const std::string &path, const char *mode) {
#ifdef _WIN32
    std::wstring wm(mode, mode + strlen(mode));
    return _wfopen(u8path(path).c_str(), wm.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

static std::string readFile(const std::string &path) {
    FILE *f = openFile(path, "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void usage() {
    puts("fiso " FISO_LIB_VERSION
         " — FISO Open Audio Disc Format tool\n"
         "\n"
         "usage:\n"
         "  fiso info    IMAGE [--json]\n"
         "  fiso verify  IMAGE [--quick] [--json]\n"
         "  fiso extract IMAGE --format flac|wav|cover|meta [--out DIR]\n"
         "                    [--tracks 1,3-5] [--template \"{track:02} - {title}\"] [--include-htoa] [--overwrite]\n"
         "  fiso extract IMAGE --format bin-cue [--convert] [--out DIR] [--overwrite]\n"
         "                    (BIN/CUE for burning software; track starts moved to CD sectors if needed;\n"
         "                     --convert accepts a conversion to 44.1 kHz / 16 bit / stereo; IMAGE is not modified)\n"
         "  fiso create  OUT.fiso (--from-cue FILE.cue | --from-files A.flac B.wav ... | --plan PLAN.json)\n"
         "                    [--meta META.json] [--album T] [--artist A] [--date D] [--genre G]\n"
         "                    [--disc N] [--disc-total M] [--disc-subtitle S] [--group-id UUID]\n"
         "                    [--front IMG] [--back IMG] [--compression 0-8] [--reencode]\n"
         "                    [--no-verify] [--overwrite]\n"
         "  fiso meta get IMAGE\n"
         "  fiso meta set IMAGE [--json META.json] [--album T] [--artist A] [--date D] [--genre G]\n"
         "                    [--disc N] [--disc-total M] [--disc-subtitle S] [--title N=TEXT]\n"
         "                    [--front IMG] [--back IMG] [--remove-pictures] [--edit EDIT.json] [--out OUT.fiso] [--overwrite]\n"
         "  fiso group   IMAGE... [--json]\n"
         "  fiso discid  IMAGE\n"
         "  fiso pcm     IMAGE [--start SAMPLE] [--count N] [--format s16|s32|f32]   (raw PCM on stdout)\n"
         "  fiso accuraterip IMAGE [--db dBAR.bin] [--mark] [--json]\n"
         "  fiso thumbnail IMAGE OUT   (front cover, for desktop thumbnailers)\n"
         "\n"
         "CD drives (macOS):\n"
         "  fiso drives  [--json]\n"
         "  fiso cd-info [--device ID] [--json]\n"
         "  fiso rip     OUT.fiso [--options OPTS.json] [--device ID] [--fast] [--offset SAMPLES] [--retries N] [--no-pregaps] "
         "[--no-htoa]\n"
         "               [--meta META.json] [--album T] [--artist A] [--disc N] [--disc-total M] [--front IMG] [--overwrite]\n"
         "  fiso eject   [--device ID]\n"
         "\n"
         "common: --progress-json  machine-readable progress/errors on stderr\n"
         "exit codes: 0 ok, 1 usage, 2 I/O, 3 invalid format, 4 integrity failure, 5 unsupported,\n"
         "            6 output exists, 7 cancelled, 8 FLAC error, 9 internal");
}

struct Args {
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, std::string>> opt;  // repeated options allowed
    std::vector<std::string> files;                        // --from-files list
    bool has(const std::string &k) const {
        for (auto &o : opt)
            if (o.first == k) return true;
        return false;
    }
    std::string get(const std::string &k, const std::string &d = "") const {
        for (auto &o : opt)
            if (o.first == k) return o.second;
        return d;
    }
};

static const char *kFlags[] = {"--json",           "--quick",    "--overwrite",  "--include-htoa",
                               "--no-passthrough", "--reencode", "--no-verify",  "--remove-pictures",
                               "--progress-json",  "--fast",     "--no-pregaps", "--no-htoa",
                               "--mark",           "--convert",  nullptr};

static bool isFlag(const std::string &s) {
    for (int i = 0; kFlags[i]; i++)
        if (s == kFlags[i]) return true;
    return false;
}

static bool parseArgs(int argc, char **argv, int start, Args &a) {
    for (int i = start; i < argc; i++) {
        std::string s = argv[i];
        if (s == "--from-files") {
            while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) a.files.push_back(argv[++i]);
            a.opt.push_back({s, ""});
        } else if (s.rfind("--", 0) == 0) {
            if (isFlag(s))
                a.opt.push_back({s, "1"});
            else if (i + 1 < argc)
                a.opt.push_back({s, argv[++i]});
            else {
                fprintf(stderr, "fiso: option %s needs a value\n", s.c_str());
                return false;
            }
        } else {
            a.pos.push_back(s);
        }
    }
    g_progress_json = a.has("--progress-json");
    return true;
}

static std::vector<int> parseTracks(const std::string &s) {
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(',', i);
        std::string part = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
        auto dash = part.find('-');
        if (dash != std::string::npos) {
            int a = atoi(part.substr(0, dash).c_str()), b = atoi(part.substr(dash + 1).c_str());
            for (int k = a; k <= b; k++) v.push_back(k);
        } else if (!part.empty()) {
            v.push_back(atoi(part.c_str()));
        }
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return v;
}

static void applyMetaOptions(json &m, const Args &a) {
    auto setAlbum = [&](const char *opt, const char *key) {
        if (a.has(opt)) m["album"][key] = a.get(opt);
    };
    setAlbum("--album", "title");
    setAlbum("--artist", "artist");
    setAlbum("--date", "date");
    setAlbum("--genre", "genre");
    setAlbum("--group-id", "group_id");
    if (a.has("--disc")) m["disc"]["number"] = atoi(a.get("--disc").c_str());
    if (a.has("--disc-total")) m["disc"]["total"] = atoi(a.get("--disc-total").c_str());
    if (a.has("--disc-subtitle")) m["disc"]["subtitle"] = a.get("--disc-subtitle");
    for (auto &o : a.opt) {
        if (o.first != "--title") continue;
        auto eq = o.second.find('=');
        if (eq == std::string::npos) continue;
        int n = atoi(o.second.substr(0, eq).c_str());
        if (!m.contains("tracks")) m["tracks"] = json::array();
        bool found = false;
        for (auto &t : m["tracks"])
            if (t.value("number", -1) == n) {
                t["title"] = o.second.substr(eq + 1);
                found = true;
            }
        if (!found) m["tracks"].push_back({{"number", n}, {"title", o.second.substr(eq + 1)}});
    }
}

static std::string fmtDur(double s) {
    char b[32];
    int t = (int)(s + 0.5);
    snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

static int cmdInfo(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    fiso_image *img;
    int st = fiso_open(a.pos[0].c_str(), &img);
    if (st) return die(st);
    char *p = fiso_info_json(img);
    fiso_close(img);
    if (!p) return die(FISO_E_INTERNAL);
    std::string s = take(&p);
    if (a.has("--json")) {
        puts(s.c_str());
        return 0;
    }
    json j = json::parse(s);
    auto &m = j["meta"];
    printf("Album        : %s\n", m["album"].value("title", "").c_str());
    printf("Artist       : %s\n", m["album"].value("artist", "").c_str());
    if (m["album"].contains("date")) printf("Date         : %s\n", m["album"]["date"].get<std::string>().c_str());
    if (m["album"].contains("genre")) printf("Genre        : %s\n", m["album"]["genre"].get<std::string>().c_str());
    printf("Disc         : %d / %d%s\n", m["disc"].value("number", 1), m["disc"].value("total", 1),
           m["disc"].contains("subtitle") ? (" — " + m["disc"]["subtitle"].get<std::string>()).c_str() : "");
    auto &au = j["audio"];
    printf("Audio        : FLAC, %u Hz, %u bit, %u ch, %s\n", au["sample_rate"].get<unsigned>(),
           au["bits_per_sample"].get<unsigned>(), au["channels"].get<unsigned>(), fmtDur(au["duration_seconds"]).c_str());
    printf("CD-DA        : %s%s\n", j["cdda"].get<bool>() ? "yes" : "no",
           j["htoa"].get<bool>() ? ", hidden track one audio" : "");
    auto &sz = j["sizes"];
    printf("Size         : %.1f MB (PCM %.1f MB, %.1f %%)\n", sz["file_bytes"].get<double>() / 1e6,
           sz["pcm_bytes"].get<double>() / 1e6, sz["ratio"].get<double>() * 100);
    if (j.contains("musicbrainz_disc_id")) printf("MB disc id   : %s\n", j["musicbrainz_disc_id"].get<std::string>().c_str());
    printf("Pictures     : %zu\n", j["pictures"].size());
    printf("\n  #  %-40s %8s %8s %10s\n", "Title", "Length", "Pregap", "FLAC");
    for (auto &t : j["tracks"]) {
        std::string title = t.value("title", "");
        if (title.empty()) title = "(untitled)";
        printf(" %2d  %-40.40s %8s %8s %8.1f MB\n", t["number"].get<int>(), title.c_str(), fmtDur(t["duration_seconds"]).c_str(),
               t["pregap_samples"].get<uint64_t>()
                   ? fmtDur(t["pregap_samples"].get<double>() / au["sample_rate"].get<double>()).c_str()
                   : "-",
               t["compressed_bytes"].get<double>() / 1e6);
    }
    for (auto &w : j["warnings"]) printf("warning: %s\n", w.get<std::string>().c_str());
    return 0;
}

static int cmdVerify(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    char *r = nullptr;
    int st = fiso_verify(a.pos[0].c_str(), a.has("--quick") ? 0 : 1, progressCb, nullptr, &r);
    endProgress();
    if (st) return die(st);
    std::string s = take(&r);
    json j = json::parse(s);
    bool vf = j["valid_file"]["ok"].get<bool>();
    bool ia_checked = j["intact_audio"]["checked"].get<bool>();
    bool ia = ia_checked && j["intact_audio"]["ok"].is_boolean() && j["intact_audio"]["ok"].get<bool>();
    if (a.has("--json"))
        puts(s.c_str());
    else {
        printf("Valid file   : %s\n", vf ? "yes" : "NO");
        for (auto &e : j["valid_file"]["errors"]) printf("  - %s\n", e.get<std::string>().c_str());
        printf("Intact audio : %s\n", !ia_checked ? "not checked (--quick)" : ia ? "yes (FLAC MD5 + PCM SHA-256)" : "NO");
        for (auto &e : j["intact_audio"]["errors"]) printf("  - %s\n", e.get<std::string>().c_str());
        printf("Verified rip : %s\n", j["verified_rip"]["state"] == "verified"
                                          ? ("yes — " + j["verified_rip"]["method"].get<std::string>()).c_str()
                                          : "no (no external reference recorded)");
    }
    if (!vf) return FISO_E_FORMAT;
    if (ia_checked && !ia) return FISO_E_INTEGRITY;
    return 0;
}

static int cmdExtract(const Args &a) {
    if (a.pos.size() != 1 || !a.has("--format")) {
        usage();
        return 1;
    }
    json o = {{"format", a.get("--format")},
              {"out_dir", a.get("--out", ".")},
              {"overwrite", a.has("--overwrite")},
              {"include_htoa", a.has("--include-htoa")},
              {"allow_conversion", a.has("--convert")}};
    if (a.has("--tracks")) o["tracks"] = parseTracks(a.get("--tracks"));
    if (a.has("--template")) o["template"] = a.get("--template");
    char *r = nullptr;
    int st = fiso_extract(a.pos[0].c_str(), o.dump().c_str(), progressCb, nullptr, &r);
    endProgress();
    if (st) return die(st);
    json j = json::parse(take(&r));
    if (a.has("--json")) {
        puts(j.dump().c_str());
        return 0;
    }
    for (auto &f : j["files"]) printf("%s\n", f.get<std::string>().c_str());
    for (auto &w : j["warnings"]) fprintf(stderr, "warning: %s\n", w.get<std::string>().c_str());
    return 0;
}

static int cmdCreate(const Args &a) {
    json plan;
    if (a.has("--plan")) {
        plan = json::parse(readFile(a.get("--plan")), nullptr, false);
        if (plan.is_discarded()) return die(FISO_E_ARG, "plan is not valid JSON");
    }
    if (!a.pos.empty()) plan["output"] = a.pos[0];
    if (!plan.contains("output")) {
        usage();
        return 1;
    }
    if (a.has("--from-cue"))
        plan["source"] = {{"kind", "cue"}, {"path", a.get("--from-cue")}};
    else if (a.has("--from-files"))
        plan["source"] = {{"kind", "files"}, {"files", a.files}};
    if (!plan.contains("source")) {
        usage();
        return 1;
    }
    json m = plan.value("meta", json::object());
    if (a.has("--meta")) {
        json f = json::parse(readFile(a.get("--meta")), nullptr, false);
        if (f.is_discarded()) return die(FISO_E_ARG, "metadata file is not valid JSON");
        m = f;
    }
    applyMetaOptions(m, a);
    plan["meta"] = m;
    if (!plan.contains("pictures")) plan["pictures"] = json::array();
    if (a.has("--front")) plan["pictures"].push_back({{"type", 3}, {"path", a.get("--front")}});
    if (a.has("--back")) plan["pictures"].push_back({{"type", 4}, {"path", a.get("--back")}});
    if (a.has("--compression")) plan["compression"] = atoi(a.get("--compression").c_str());
    // --reencode: FLAC sources are decoded and encoded again with the same settings for every track
    if (a.has("--reencode") || a.has("--no-passthrough")) plan["flac_passthrough"] = false;
    if (a.has("--no-verify")) plan["verify"] = false;
    if (a.has("--overwrite")) plan["overwrite"] = true;
    char *r = nullptr;
    int st = fiso_create(plan.dump().c_str(), progressCb, nullptr, &r);
    endProgress();
    if (st) return die(st);
    std::string s = take(&r);
    json j = json::parse(s);
    if (a.has("--json") || g_progress_json) {
        puts(s.c_str());
    } else {
        printf("Created      : %s\n", j["output"].get<std::string>().c_str());
        printf("Tracks       : %d, %s\n", j["tracks"].get<int>(), fmtDur(j["duration_seconds"]).c_str());
        printf("PCM size     : %.1f MB\n", j["pcm_bytes"].get<double>() / 1e6);
        printf("FISO size    : %.1f MB (%.1f %% of PCM)\n", j["fiso_bytes"].get<double>() / 1e6, j["ratio"].get<double>() * 100);
        printf("CD-DA        : %s\n", j["cdda"].get<bool>() ? "yes" : "no");
        if (!j["flac_passthrough_tracks"].empty())
            printf("FLAC copied  : %zu track(s) stored without re-encoding\n", j["flac_passthrough_tracks"].size());
        else
            printf("Encoding     : every track encoded with %s, level %d\n", j["encoding"]["encoder"].get<std::string>().c_str(),
                   j["encoding"]["compression"].get<int>());
        if (j.contains("verify"))
            printf("Verification : %s\n",
                   j["verify"]["valid_file"]["ok"].get<bool>() && j["verify"]["intact_audio"]["ok"] == true ? "OK" : "FAILED");
        for (auto &w : j["warnings"]) printf("warning: %s\n", w.get<std::string>().c_str());
    }
    if (j.contains("verify") && !(j["verify"]["intact_audio"]["ok"] == true)) return FISO_E_INTEGRITY;
    return 0;
}

static int cmdMeta(const Args &a) {
    if (a.pos.size() != 2) {
        usage();
        return 1;
    }
    const std::string &sub = a.pos[0], &path = a.pos[1];
    fiso_image *img;
    int st = fiso_open(path.c_str(), &img);
    if (st) return die(st);
    char *p = fiso_meta_json(img);
    fiso_close(img);
    json m = json::parse(take(&p));
    if (sub == "get") {
        puts(m.dump(2).c_str());
        return 0;
    }
    if (sub != "set") {
        usage();
        return 1;
    }
    if (a.has("--json")) {
        m = json::parse(readFile(a.get("--json")), nullptr, false);
        if (m.is_discarded()) return die(FISO_E_ARG, "metadata file is not valid JSON");
    }
    applyMetaOptions(m, a);
    json e = {{"meta", m},
              {"pictures", json::array()},
              {"remove_all_pictures", a.has("--remove-pictures")},
              {"overwrite", a.has("--overwrite")}};
    if (a.has("--edit")) {  // full edit document, see fiso_edit()
        e = json::parse(readFile(a.get("--edit")), nullptr, false);
        if (e.is_discarded()) return die(FISO_E_ARG, "edit file is not valid JSON");
        if (a.has("--overwrite")) e["overwrite"] = true;
    }
    if (a.has("--front")) e["pictures"].push_back({{"type", 3}, {"path", a.get("--front")}});
    if (a.has("--back")) e["pictures"].push_back({{"type", 4}, {"path", a.get("--back")}});
    std::string out = a.get("--out");
    st = fiso_edit(path.c_str(), e.dump().c_str(), out.empty() ? nullptr : out.c_str(), progressCb, nullptr);
    endProgress();
    if (st) return die(st);
    return 0;
}

static int cmdGroup(const Args &a) {
    if (a.pos.empty()) {
        usage();
        return 1;
    }
    char *r = nullptr;
    int st = fiso_group(json(a.pos).dump().c_str(), &r);
    if (st) return die(st);
    std::string s = take(&r);
    if (a.has("--json")) {
        puts(s.c_str());
        return 0;
    }
    json j = json::parse(s);
    for (auto &g : j["groups"]) {
        printf("Album: %s — %s  [%s]\n", g["album"].get<std::string>().c_str(), g["artist"].get<std::string>().c_str(),
               g["method"].get<std::string>().c_str());
        for (auto &d : g["discs"])
            printf("  Disc %d — %zu tracks  %s\n", d["number"].get<int>(), d["tracks"].get<size_t>(),
                   d["path"].get<std::string>().c_str());
        if (!g["missing_discs"].empty()) printf("  missing discs: %s\n", g["missing_discs"].dump().c_str());
    }
    for (auto &e : j["errors"])
        fprintf(stderr, "error: %s: %s\n", e["path"].get<std::string>().c_str(), e["error"].get<std::string>().c_str());
    return 0;
}

static int cmdDiscid(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    fiso_image *img;
    int st = fiso_open(a.pos[0].c_str(), &img);
    if (st) return die(st);
    char *p = fiso_info_json(img);
    fiso_close(img);
    json j = json::parse(take(&p));
    if (!j.contains("musicbrainz_disc_id")) return die(FISO_E_UNSUPPORTED, "no disc id: image is not CD-DA conformant");
    printf("%s\n%s\n", j["musicbrainz_disc_id"].get<std::string>().c_str(), j["musicbrainz_toc"].get<std::string>().c_str());
    return 0;
}

static int cmdDrives(const Args &a) {
    char *r = nullptr;
    int st = fiso_cd_drives(&r);
    if (st) return die(st);
    std::string s = take(&r);
    if (a.has("--json")) {
        puts(s.c_str());
        return 0;
    }
    json j = json::parse(s);
    if (j["drives"].empty()) {
        puts(j["platform"] == "unsupported" ? "CD drives are supported on macOS only." : "No optical drive found.");
        return 0;
    }
    for (auto &d : j["drives"]) {
        auto &m = d["media"];
        std::string media = m.value("state", "") != "present" ? "no disc"
                            : m.value("blank", false)
                                ? "blank " + m.value("type", std::string("disc"))
                                : m.value("type", std::string("disc")) +
                                      (m.value("bsd_name", "").empty() ? "" : " (" + m["bsd_name"].get<std::string>() + ")");
        printf("%-8s %-40s %s%s\n", d["id"].get<std::string>().c_str(), d["name"].get<std::string>().c_str(), media.c_str(), "");
    }
    return 0;
}

static int cmdCdInfo(const Args &a) {
    char *r = nullptr;
    int st = fiso_cd_info(a.get("--device").c_str(), &r);
    if (st) return die(st);
    std::string s = take(&r);
    if (a.has("--json")) {
        puts(s.c_str());
        return 0;
    }
    json j = json::parse(s);
    printf("Drive        : %s\n", j["drive"].get<std::string>().c_str());
    printf("Audio tracks : %d, %s\n", j["audio_tracks"].get<int>(), fmtDur(j["seconds"].get<double>()).c_str());
    if (!j["mcn"].get<std::string>().empty()) printf("MCN          : %s\n", j["mcn"].get<std::string>().c_str());
    if (j.contains("musicbrainz_disc_id")) printf("MB disc id   : %s\n", j["musicbrainz_disc_id"].get<std::string>().c_str());
    if (j.contains("accuraterip")) printf("AccurateRip  : %s\n", j["accuraterip"]["url"].get<std::string>().c_str());
    if (j["cdtext"].contains("album_title")) printf("CD-Text      : %s\n", j["cdtext"]["album_title"].get<std::string>().c_str());
    for (auto &t : j["tracks"])
        printf(" %2d  %s  LBA %7d  %s%s\n", t["number"].get<int>(), t["audio"].get<bool>() ? "audio" : "data ",
               t["lba"].get<int>(), fmtDur(t["seconds"].get<double>()).c_str(),
               t.contains("isrc") && !t["isrc"].get<std::string>().empty() ? ("  ISRC " + t["isrc"].get<std::string>()).c_str()
                                                                           : "");
    return 0;
}

static int cmdRip(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    json o = {{"output", a.pos[0]},
              {"device", a.get("--device")},
              {"mode", a.has("--fast") ? "fast" : "secure"},
              {"read_offset", atoi(a.get("--offset", "0").c_str())},
              {"overwrite", a.has("--overwrite")},
              {"detect_pregaps", !a.has("--no-pregaps")},
              {"read_htoa", !a.has("--no-htoa")}};
    if (a.has("--retries")) o["retries"] = atoi(a.get("--retries").c_str());
    if (a.has("--compression")) o["compression"] = atoi(a.get("--compression").c_str());
    json m = json::object();
    if (a.has("--options")) {  // complete options document (see fiso_cd_rip)
        json f = json::parse(readFile(a.get("--options")), nullptr, false);
        if (f.is_discarded()) return die(FISO_E_ARG, "options file is not valid JSON");
        for (auto &[k, v] : f.items())
            if (k != "output") o[k] = v;
        m = o.value("meta", json::object());
    }
    if (a.has("--meta")) {
        m = json::parse(readFile(a.get("--meta")), nullptr, false);
        if (m.is_discarded()) return die(FISO_E_ARG, "metadata file is not valid JSON");
    }
    applyMetaOptions(m, a);
    o["meta"] = m;
    if (!o.contains("pictures")) o["pictures"] = json::array();
    if (a.has("--front")) o["pictures"].push_back({{"type", 3}, {"path", a.get("--front")}});
    if (a.has("--back")) o["pictures"].push_back({{"type", 4}, {"path", a.get("--back")}});
    char *r = nullptr;
    int st = fiso_cd_rip(o.dump().c_str(), progressCb, nullptr, &r);
    endProgress();
    if (st) return die(st);
    std::string s = take(&r);
    json j = json::parse(s);
    if (a.has("--json") || g_progress_json)
        puts(s.c_str());
    else {
        printf("Created      : %s\n", j["output"].get<std::string>().c_str());
        printf("Read errors  : %zu unrecovered sector(s), %llu re-read\n", j["rip"]["unrecovered_sectors"].size(),
               (unsigned long long)j["rip"]["retried_sectors"].get<uint64_t>());
        printf("FISO size    : %.1f MB (%.1f %% of PCM)\n", j["fiso_bytes"].get<double>() / 1e6, j["ratio"].get<double>() * 100);
        for (auto &w : j["warnings"]) printf("warning: %s\n", w.get<std::string>().c_str());
    }
    if (!j["rip"]["unrecovered_sectors"].empty()) return FISO_E_INTEGRITY;
    return 0;
}

static int cmdEject(const Args &a) {
    int st = fiso_cd_eject(a.get("--device").c_str());
    return st ? die(st) : 0;
}

static int cmdAccurateRip(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    std::string db = a.get("--db");
    char *r = nullptr;
    int st = fiso_accuraterip(a.pos[0].c_str(), db.empty() ? nullptr : db.c_str(), progressCb, nullptr, &r);
    endProgress();
    if (st) return die(st);
    std::string s = take(&r);
    json j = json::parse(s);
    bool all = j.contains("result") && j["result"]["all_matched"] == true;
    if (a.has("--mark") && all) {
        fiso_image *img;
        if ((st = fiso_open(a.pos[0].c_str(), &img))) return die(st);
        char *mp = fiso_meta_json(img);
        fiso_close(img);
        json m = json::parse(take(&mp));
        auto &res = j["result"];
        bool v2 = true;
        for (auto &t : res["tracks"])
            if (t["match"] != "v2") v2 = false;
        char buf[160];
        snprintf(buf, sizeof buf, "AccurateRip %s: %zu/%zu tracks matched, minimum confidence %u", v2 ? "v2" : "v1/v2",
                 res["tracks"].size(), res["tracks"].size(), res["min_confidence"].get<unsigned>());
        if (!m.contains("provenance")) m["provenance"] = json::object();
        if (!m["provenance"].contains("rip")) m["provenance"]["rip"] = json::object();
        m["provenance"]["rip"]["verified"] = true;
        m["provenance"]["rip"]["verification"] = buf;
        st = fiso_edit(a.pos[0].c_str(), json({{"meta", m}}).dump().c_str(), nullptr, nullptr, nullptr);
        if (st) return die(st);
        j["marked"] = true;
    }
    if (a.has("--json")) {
        puts(j.dump().c_str());
        return all || !j.contains("result") ? 0 : FISO_E_INTEGRITY;
    }
    printf("URL          : %s\n", j["url"].get<std::string>().c_str());
    for (auto &c : j["crcs"]) {
        std::string m = "";
        if (j.contains("result"))
            for (auto &t : j["result"]["tracks"])
                if (t["number"] == c["number"])
                    m = t["match"].is_null() ? "  no match"
                                             : "  match " + t["match"].get<std::string>() + " (confidence " +
                                                   std::to_string(t["confidence"].get<int>()) + ")";
        printf(" %2d  v1 %s  v2 %s%s\n", c["number"].get<int>(), c["v1"].get<std::string>().c_str(),
               c["v2"].get<std::string>().c_str(), m.c_str());
    }
    if (j.contains("result"))
        printf("Result       : %s\n", !j["result"]["in_database"].get<bool>() ? "disc not in the AccurateRip database"
                                      : all                                   ? "all tracks verified"
                                                                              : "NOT all tracks match");
    return all || !j.contains("result") ? 0 : FISO_E_INTEGRITY;
}

// Writes the front cover (or the first picture) to OUT, for desktop thumbnailers.
static int cmdThumbnail(const Args &a) {
    if (a.pos.size() != 2) {
        usage();
        return 1;
    }
    char *r = nullptr;
    std::string dir = a.pos[1] + ".d";
    json o = {{"format", "cover"}, {"out_dir", dir}, {"overwrite", true}};
    int st = fiso_extract(a.pos[0].c_str(), o.dump().c_str(), nullptr, nullptr, &r);
    if (st) return die(st);
    json j = json::parse(take(&r));
    std::string pick;
    for (auto &f : j["files"]) {
        std::string p = f.get<std::string>();
        if (pick.empty() || p.find("front_cover") != std::string::npos) pick = p;
        if (p.find("front_cover") != std::string::npos) break;
    }
    int rc = 0;
    if (pick.empty())
        rc = die(FISO_E_FORMAT, "this image has no artwork");
    else {
        std::error_code ec;
        std::filesystem::rename(u8path(pick), u8path(a.pos[1]), ec);
        if (ec) rc = die(FISO_E_IO, "cannot write " + a.pos[1]);
    }
    std::error_code ignored;
    std::filesystem::remove_all(u8path(dir), ignored);
    return rc;
}

static int cmdPcm(const Args &a) {
    if (a.pos.size() != 1) {
        usage();
        return 1;
    }
    fiso_image *img;
    int st = fiso_open(a.pos[0].c_str(), &img);
    if (st) return die(st);
    fiso_format f;
    fiso_get_format(img, &f);
    uint64_t start = strtoull(a.get("--start", "0").c_str(), nullptr, 10);
    uint64_t count = a.has("--count") ? strtoull(a.get("--count").c_str(), nullptr, 10) : f.total_samples;
    if (start > f.total_samples) start = f.total_samples;
    if (count > f.total_samples - start) count = f.total_samples - start;
    std::string fmt = a.get("--format", "s16");
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::vector<int32_t> buf(16384 * f.channels);
    std::vector<uint8_t> out;
    while (count && !g_cancel) {
        uint64_t n = count < 16384 ? count : 16384;
        int64_t got = fiso_read_pcm_s32(img, start, n, buf.data());
        if (got <= 0) {
            fiso_close(img);
            return die(got < 0 ? (int)-got : FISO_E_INTEGRITY);
        }
        size_t ns = (size_t)got * f.channels;
        out.clear();
        for (size_t i = 0; i < ns; i++) {
            int32_t v = buf[i];
            if (fmt == "f32") {
                float x = (float)v / (float)(1u << (f.bits_per_sample - 1));
                uint8_t b[4];
                memcpy(b, &x, 4);
                out.insert(out.end(), b, b + 4);
            } else if (fmt == "s32") {
                uint32_t u = (uint32_t)v << (32 - f.bits_per_sample);
                for (int k = 0; k < 4; k++) out.push_back((u >> (8 * k)) & 0xff);
            } else {
                int32_t s = f.bits_per_sample >= 16 ? v >> (f.bits_per_sample - 16) : v << (16 - f.bits_per_sample);
                out.push_back(s & 0xff);
                out.push_back((s >> 8) & 0xff);
            }
        }
        if (fwrite(out.data(), 1, out.size(), stdout) != out.size()) break;  // reader closed the pipe
        start += got;
        count -= got;
    }
    fflush(stdout);
    fiso_close(img);
    return 0;
}

#ifdef _WIN32
// The C runtime gives main() its arguments in the ANSI code page, which cannot represent
// every file name. Rebuild them as UTF-8 from the UTF-16 command line.
static std::vector<std::string> g_args;
static std::vector<char *> g_argv;
static void utf8Arguments(int &argc, char **&argv) {
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return;
    for (int i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        g_args.push_back(s);
    }
    LocalFree(w);
    for (auto &a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    argc = n;
    argv = g_argv.data();
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    utf8Arguments(argc, argv);
    _setmode(_fileno(stdout), _O_BINARY);  // byte-exact output (JSON in UTF-8, raw PCM)
#endif
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    g_tty = ISATTY(FILENO(stderr));
    if (argc < 2) {
        usage();
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "version") {
        printf("fiso %s (libfiso %s, format %d.%d)\n", FISO_LIB_VERSION, fiso_version(), FISO_FORMAT_VERSION_MAJOR,
               FISO_FORMAT_VERSION_MINOR);
        return 0;
    }
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }
    Args a;
    if (!parseArgs(argc, argv, 2, a)) return 1;
    try {
        if (cmd == "info") return cmdInfo(a);
        if (cmd == "verify") return cmdVerify(a);
        if (cmd == "extract") return cmdExtract(a);
        if (cmd == "create") return cmdCreate(a);
        if (cmd == "meta") return cmdMeta(a);
        if (cmd == "group") return cmdGroup(a);
        if (cmd == "discid") return cmdDiscid(a);
        if (cmd == "pcm") return cmdPcm(a);
        if (cmd == "thumbnail") return cmdThumbnail(a);
        if (cmd == "drives") return cmdDrives(a);
        if (cmd == "cd-info") return cmdCdInfo(a);
        if (cmd == "rip") return cmdRip(a);
        if (cmd == "eject") return cmdEject(a);
        if (cmd == "accuraterip") return cmdAccurateRip(a);
    } catch (const std::exception &e) {
        return die(FISO_E_INTERNAL, e.what());
    }
    usage();
    return 1;
}
