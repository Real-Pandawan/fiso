// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace fiso {

void fail(int code, const std::string &msg) {
    throw Error(code, msg);
}

// ------------------------------------------------------------------ SHA-256
static const uint32_t K256[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                                  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                                  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                                  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                                  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                                  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                                  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                                  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

Sha256::Sha256() {
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof h_);
}

void Sha256::block(const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    total_ += len;
    if (buflen_) {
        size_t n = std::min(len, 64 - buflen_);
        memcpy(buf_ + buflen_, p, n);
        buflen_ += n;
        p += n;
        len -= n;
        if (buflen_ == 64) {
            block(buf_);
            buflen_ = 0;
        }
    }
    while (len >= 64) {
        block(p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(buf_, p, len);
        buflen_ = len;
    }
}

Hash Sha256::finish() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (buflen_ != 56) update(&z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lenb, 8);
    Hash out;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h_[i] >> (24 - 8 * j));
    return out;
}

// ------------------------------------------------------------------ SHA-1 (MusicBrainz disc id)
std::string sha1_raw(const std::string &msg) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    uint64_t bits = (uint64_t)msg.size() * 8;
    m.push_back((char)0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((char)(bits >> (8 * i)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = (const uint8_t *)m.data() + off + 4 * i;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (int i = 0; i < 5; i++)
        for (int j = 3; j >= 0; j--) out.push_back((char)(h[i] >> (8 * j)));
    return out;
}

// ------------------------------------------------------------------ CRC-32
uint32_t crc32(const void *data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    const uint8_t *p = (const uint8_t *)data;
    while (len--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::string hex(const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

std::string getstr(const uint8_t *p, size_t field) {
    size_t n = 0;
    while (n < field && p[n]) n++;
    return std::string((const char *)p, n);
}

// ------------------------------------------------------------------ File
static fs::path P(const std::string &s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

File::~File() {
    if (f_) fclose(f_);
}
File &File::operator=(File &&o) noexcept {
    if (this != &o) {
        if (f_) fclose(f_);
        f_ = o.f_;
        path_ = std::move(o.path_);
        o.f_ = nullptr;
    }
    return *this;
}

File File::open(const std::string &path, const char *mode) {
    File r;
#ifdef _WIN32
    std::wstring wm(mode, mode + strlen(mode));
    r.f_ = _wfopen(P(path).c_str(), wm.c_str());
#else
    r.f_ = fopen(path.c_str(), mode);
#endif
    if (!r.f_) fail(FISO_E_IO, "cannot open '" + path + "': " + strerror(errno));
    r.path_ = path;
    return r;
}

void File::seek(uint64_t pos) {
#ifdef _WIN32
    if (_fseeki64(f_, (int64_t)pos, SEEK_SET) != 0)
#else
    if (fseeko(f_, (off_t)pos, SEEK_SET) != 0)
#endif
        fail(FISO_E_IO, "seek failed in '" + path_ + "'");
}
uint64_t File::tell() {
#ifdef _WIN32
    return (uint64_t)_ftelli64(f_);
#else
    return (uint64_t)ftello(f_);
#endif
}
uint64_t File::size() {
    uint64_t cur = tell();
#ifdef _WIN32
    _fseeki64(f_, 0, SEEK_END);
#else
    fseeko(f_, 0, SEEK_END);
#endif
    uint64_t s = tell();
    seek(cur);
    return s;
}
void File::read(void *buf, size_t n) {
    if (n && fread(buf, 1, n, f_) != n) fail(FISO_E_FORMAT, "unexpected end of file in '" + path_ + "'");
}
size_t File::readSome(void *buf, size_t n) {
    return n ? fread(buf, 1, n, f_) : 0;
}
void File::write(const void *buf, size_t n) {
    if (n && fwrite(buf, 1, n, f_) != n) fail(FISO_E_IO, "write failed in '" + path_ + "': " + strerror(errno));
}
void File::sync() {
    if (fflush(f_) != 0) fail(FISO_E_IO, "flush failed in '" + path_ + "'");
#ifdef _WIN32
    _commit(_fileno(f_));
#elif defined(__APPLE__)
    fcntl(fileno(f_), F_FULLFSYNC);
#else
    fsync(fileno(f_));
#endif
}
void File::close() {
    if (f_) {
        int r = fclose(f_);
        f_ = nullptr;
        if (r != 0) fail(FISO_E_IO, "close failed in '" + path_ + "'");
    }
}

bool pathExists(const std::string &p) {
    std::error_code ec;
    return fs::exists(P(p), ec);
}
void removeFile(const std::string &p) noexcept {
    std::error_code ec;
    fs::remove(P(p), ec);
}

void renameReplace(const std::string &from, const std::string &to, bool overwrite) {
    if (!overwrite && pathExists(to)) fail(FISO_E_EXISTS, "'" + to + "' already exists");
    std::error_code ec;
    fs::rename(P(from), P(to), ec);  // atomic on the same volume (POSIX rename / MoveFileEx)
    if (ec) fail(FISO_E_IO, "cannot rename to '" + to + "': " + ec.message());
}

std::string tempPathFor(const std::string &target) {
    std::random_device rd;
    char buf[32];
    snprintf(buf, sizeof buf, ".%08x.tmp", (unsigned)rd());
    std::string dir = dirName(target), base = baseName(target);
    return joinPath(dir, "." + base + buf);
}
std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    auto r = (P(dir) / P(name)).u8string();
    return std::string(r.begin(), r.end());
}
std::string dirName(const std::string &p) {
    auto r = P(p).parent_path().u8string();
    return std::string(r.begin(), r.end());
}
std::string baseName(const std::string &p) {
    auto r = P(p).filename().u8string();
    return std::string(r.begin(), r.end());
}
void makeDirs(const std::string &p) {
    std::error_code ec;
    fs::create_directories(P(p), ec);
    if (ec) fail(FISO_E_IO, "cannot create directory '" + p + "': " + ec.message());
}
std::vector<uint8_t> readWholeFile(const std::string &p, uint64_t max) {
    File f = File::open(p, "rb");
    uint64_t n = f.size();
    if (n > max) fail(FISO_E_ARG, "file too large: '" + p + "'");
    std::vector<uint8_t> v((size_t)n);
    f.read(v.data(), v.size());
    return v;
}

// ------------------------------------------------------------------ misc
std::string uuid4() {
    std::random_device rd;
    uint8_t b[16];
    for (auto &x : b) x = (uint8_t)rd();
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    std::string h = hex(b, 16);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::string nowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string sanitizeFilename(const std::string &in) {
    std::string s;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|", c))
            s.push_back('_');
        else
            s.push_back((char)c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) s = "_";
    if (s.size() > 180) {  // keep room for extensions, cut on a UTF-8 boundary
        size_t n = 180;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s.resize(n);
    }
    static const char *reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "LPT1"};
    for (auto r : reserved)
        if (s == r) s = "_" + s;
    return s;
}

std::string msf(uint64_t sectors) {
    char buf[32];
    snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(sectors / 4500),
             (unsigned long long)(sectors / 75 % 60), (unsigned long long)(sectors % 75));
    return buf;
}

void canonicalBytes(const int32_t *s, size_t n, uint32_t bps, std::vector<uint8_t> &out) {
    uint32_t bytes = (bps + 7) / 8;
    size_t base = out.size();
    out.resize(base + n * bytes);
    uint8_t *p = out.data() + base;
    if (bytes == 2) {
        for (size_t i = 0; i < n; i++) {
            uint32_t v = (uint32_t)s[i];
            p[0] = v & 0xff;
            p[1] = (v >> 8) & 0xff;
            p += 2;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            uint32_t v = (uint32_t)s[i];
            for (uint32_t b = 0; b < bytes; b++) *p++ = (v >> (8 * b)) & 0xff;
        }
    }
}

}  // namespace fiso
