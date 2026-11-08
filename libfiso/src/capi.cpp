// C API — SPDX-License-Identifier: Apache-2.0
#include "cd.h"

#include <cstdlib>
#include <cstring>

using namespace fiso;

struct fiso_image {
    std::unique_ptr<Image> img;
};

static thread_local std::string g_err;

static char *dupstr(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (p) memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

template <class F>
static int guard(F &&f) {
    try {
        g_err.clear();
        f();
        return FISO_OK;
    } catch (const Error &e) {
        g_err = e.what();
        return e.code;
    } catch (const json::exception &e) {
        g_err = std::string("invalid JSON argument: ") + e.what();
        return FISO_E_ARG;
    } catch (const std::bad_alloc &) {
        g_err = "out of memory";
        return FISO_E_INTERNAL;
    } catch (const std::exception &e) {
        g_err = e.what();
        return FISO_E_INTERNAL;
    }
}

static json parseArg(const char *s) {
    if (!s) fail(FISO_E_ARG, "missing JSON argument");
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded()) fail(FISO_E_ARG, "argument is not valid JSON");
    return j;
}

extern "C" {

const char *fiso_version(void) {
    return FISO_LIB_VERSION;
}
const char *fiso_last_error(void) {
    return g_err.c_str();
}
void fiso_free(void *p) {
    free(p);
}

const char *fiso_status_string(int s) {
    switch (s) {
        case FISO_OK:
            return "ok";
        case FISO_E_ARG:
            return "invalid argument";
        case FISO_E_IO:
            return "I/O error";
        case FISO_E_FORMAT:
            return "invalid format";
        case FISO_E_INTEGRITY:
            return "integrity check failed";
        case FISO_E_UNSUPPORTED:
            return "unsupported";
        case FISO_E_EXISTS:
            return "output already exists";
        case FISO_E_CANCELLED:
            return "cancelled";
        case FISO_E_FLAC:
            return "FLAC codec error";
        default:
            return "internal error";
    }
}

int fiso_open(const char *path, fiso_image **out) {
    if (!path || !out) return FISO_E_ARG;
    *out = nullptr;
    return guard([&] {
        auto h = new fiso_image;
        try {
            h->img = Image::open(path);
        } catch (...) {
            delete h;
            throw;
        }
        *out = h;
    });
}

void fiso_close(fiso_image *img) {
    delete img;
}

char *fiso_info_json(fiso_image *img) {
    if (!img) return nullptr;
    char *r = nullptr;
    guard([&] { r = dupstr(infoJson(*img->img).dump()); });
    return r;
}

char *fiso_meta_json(fiso_image *img) {
    return img ? dupstr(img->img->meta_raw) : nullptr;
}

int fiso_get_format(fiso_image *img, fiso_format *f) {
    if (!img || !f) return FISO_E_ARG;
    auto &L = img->img->layout;
    f->sample_rate = L.rate;
    f->channels = L.channels;
    f->bits_per_sample = L.bps;
    f->total_samples = L.total;
    f->cdda = L.cdda() ? 1 : 0;
    return FISO_OK;
}

int64_t fiso_read_pcm_s32(fiso_image *img, uint64_t start, uint64_t count, int32_t *out) {
    if (!img || !out) return -FISO_E_ARG;
    int64_t n = 0;
    int st = guard([&] { n = (int64_t)img->img->readPcm(start, (size_t)count, out); });
    return st == FISO_OK ? n : -st;
}

int fiso_verify(const char *path, int deep, fiso_progress_fn cb, void *user, char **report) {
    if (!path || !report) return FISO_E_ARG;
    *report = nullptr;
    return guard([&] {
        Progress p{cb, user};
        *report = dupstr(verifyImage(path, deep != 0, p).dump());
    });
}

int fiso_create(const char *plan, fiso_progress_fn cb, void *user, char **report) {
    if (report) *report = nullptr;
    return guard([&] {
        Progress p{cb, user};
        json r = createImage(parseArg(plan), p);
        if (report) *report = dupstr(r.dump());
    });
}

int fiso_edit(const char *path, const char *edit, const char *out_path, fiso_progress_fn cb, void *user) {
    if (!path) return FISO_E_ARG;
    return guard([&] {
        Progress p{cb, user};
        editImage(path, parseArg(edit), out_path ? out_path : "", p);
    });
}

int fiso_extract(const char *path, const char *opts, fiso_progress_fn cb, void *user, char **result) {
    if (!path) return FISO_E_ARG;
    if (result) *result = nullptr;
    return guard([&] {
        Progress p{cb, user};
        json r = extractImage(path, parseArg(opts), p);
        if (result) *result = dupstr(r.dump());
    });
}

int fiso_group(const char *paths, char **result) {
    if (!result) return FISO_E_ARG;
    *result = nullptr;
    return guard([&] {
        json a = parseArg(paths);
        std::vector<std::string> v;
        for (auto &x : a) v.push_back(x.get<std::string>());
        *result = dupstr(groupImages(v).dump());
    });
}

int fiso_cd_drives(char **result) {
    if (!result) return FISO_E_ARG;
    *result = nullptr;
    return guard([&] { *result = dupstr(platformDrives().dump()); });
}

int fiso_cd_info(const char *device, char **result) {
    if (!result) return FISO_E_ARG;
    *result = nullptr;
    return guard([&] {
        auto r = platformOpenReader(device ? device : "");
        *result = dupstr(cd::discInfo(*r).dump());
    });
}

int fiso_cd_rip(const char *options, fiso_progress_fn cb, void *user, char **report) {
    if (report) *report = nullptr;
    return guard([&] {
        json o = parseArg(options);
        auto r = platformOpenReader(o.value("device", std::string()));
        json rep = cd::rip(*r, o, Progress{cb, user});
        if (report) *report = dupstr(rep.dump());
    });
}

int fiso_cd_eject(const char *device) {
    return guard([&] { platformEject(device ? device : ""); });
}

int fiso_accuraterip(const char *image, const char *db_path, fiso_progress_fn cb, void *user, char **result) {
    if (!image || !result) return FISO_E_ARG;
    *result = nullptr;
    return guard([&] {
        auto img = Image::open(image);
        cd::ArResult ar = cd::accurateRip(*img, Progress{cb, user});
        json j = {{"id1", ar.id1}, {"id2", ar.id2}, {"cddb", ar.cddb}, {"tracks", ar.tracks}, {"url", ar.url}};
        json crcs = json::array();
        for (auto &c : ar.crcs) {
            char a[9], b[9];
            snprintf(a, sizeof a, "%08x", c.v1);
            snprintf(b, sizeof b, "%08x", c.v2);
            crcs.push_back({{"number", c.number}, {"v1", a}, {"v2", b}});
        }
        j["crcs"] = crcs;
        if (db_path && *db_path) j["result"] = cd::accurateRipCompare(ar, readWholeFile(db_path, 4 << 20));
        *result = dupstr(j.dump());
    });
}

}  // extern "C"
