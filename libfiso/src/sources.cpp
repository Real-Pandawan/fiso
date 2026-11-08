// PCM sources (WAV, raw CD-DA, FLAC) — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <cstring>

namespace fiso {

// ------------------------------------------------------------------ WAV
class WavSource : public PcmSource {
  public:
    File f;
    uint64_t data_off = 0;
    uint32_t container = 0;  // bytes per sample in the file
    bool unsigned8 = false;
    std::vector<uint8_t> raw;

    void seek(uint64_t frame) override {
        if (frame > total) fail(FISO_E_ARG, "WAV seek out of range");
        f.seek(data_off + frame * channels * container);
        pos_ = frame;
    }
    size_t read(int32_t *out, size_t frames) override {
        frames = (size_t)std::min<uint64_t>(frames, total - pos_);
        size_t bytes = frames * channels * container;
        raw.resize(bytes);
        f.read(raw.data(), bytes);
        const uint8_t *p = raw.data();
        int shift = (int)(container * 8 - bps);
        size_t n = frames * channels;
        for (size_t i = 0; i < n; i++, p += container) {
            if (unsigned8) {
                out[i] = (int32_t)p[0] - 128;
                continue;
            }
            uint32_t v = 0;
            for (uint32_t b = 0; b < container; b++) v |= (uint32_t)p[b] << (8 * b);
            v <<= (32 - container * 8);  // sign-extend via left-justification
            out[i] = ((int32_t)v) >> (32 - container * 8 + shift);
        }
        pos_ += frames;
        return frames;
    }

  private:
    uint64_t pos_ = 0;
};

std::unique_ptr<PcmSource> openWav(const std::string &path) {
    auto s = std::make_unique<WavSource>();
    s->f = File::open(path, "rb");
    uint64_t fsize = s->f.size();
    uint8_t h[12];
    s->f.read(h, 12);
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        fail(FISO_E_UNSUPPORTED, "'" + path + "' is not a RIFF/WAVE file");
    bool have_fmt = false;
    uint64_t pos = 12;
    while (pos + 8 <= fsize) {
        uint8_t ch[8];
        s->f.seek(pos);
        s->f.read(ch, 8);
        uint32_t len = get32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            if (len < 16 || len > 1024) fail(FISO_E_FORMAT, "invalid WAV fmt chunk");
            std::vector<uint8_t> fmt(len);
            s->f.read(fmt.data(), len);
            uint16_t tag = get16(fmt.data());
            s->channels = get16(fmt.data() + 2);
            s->rate = get32(fmt.data() + 4);
            uint16_t align = get16(fmt.data() + 12);
            s->bps = get16(fmt.data() + 14);
            if (tag == 0xFFFE) {
                if (len < 40) fail(FISO_E_FORMAT, "invalid WAVE_FORMAT_EXTENSIBLE header");
                uint16_t valid = get16(fmt.data() + 18);
                if (get16(fmt.data() + 24) != 1) fail(FISO_E_UNSUPPORTED, "only integer PCM WAV files are supported");
                if (valid) s->bps = valid;
            } else if (tag != 1) {
                fail(FISO_E_UNSUPPORTED, "only integer PCM WAV files are supported (no float, ADPCM, MP3…)");
            }
            if (s->channels < 1 || s->channels > 8 || s->rate == 0 || s->bps < 4 || s->bps > 32 || align % s->channels)
                fail(FISO_E_FORMAT, "invalid WAV format values");
            s->container = align / s->channels;
            if (s->container * 8 < s->bps || s->container > 4) fail(FISO_E_FORMAT, "invalid WAV block alignment");
            s->unsigned8 = s->container == 1;
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) fail(FISO_E_FORMAT, "WAV data before fmt chunk");
            s->data_off = pos + 8;
            uint64_t avail = fsize - s->data_off;
            uint64_t dlen = (len == 0xFFFFFFFF || len > avail) ? avail : len;
            uint64_t frame = (uint64_t)s->channels * s->container;
            if (dlen % frame) fail(FISO_E_FORMAT, "WAV data length is not a whole number of sample frames");
            s->total = dlen / frame;
            s->seek(0);
            return s;
        }
        pos += 8 + len + (len & 1);
    }
    fail(FISO_E_FORMAT, "WAV file without data chunk");
}

// ------------------------------------------------------------------ raw CD-DA
class RawSource : public PcmSource {
  public:
    File f;
    bool be = false;
    std::vector<uint8_t> raw;
    void seek(uint64_t frame) override {
        f.seek(frame * 4);
        pos_ = frame;
    }
    size_t read(int32_t *out, size_t frames) override {
        frames = (size_t)std::min<uint64_t>(frames, total - pos_);
        raw.resize(frames * 4);
        f.read(raw.data(), raw.size());
        for (size_t i = 0; i < frames * 2; i++) {
            uint8_t a = raw[2 * i], b = raw[2 * i + 1];
            out[i] = be ? (int16_t)(a << 8 | b) : (int16_t)(b << 8 | a);
        }
        pos_ += frames;
        return frames;
    }

  private:
    uint64_t pos_ = 0;
};

std::unique_ptr<PcmSource> openRawCdda(const std::string &path, bool big_endian) {
    auto s = std::make_unique<RawSource>();
    s->f = File::open(path, "rb");
    uint64_t n = s->f.size();
    if (n % 4) fail(FISO_E_FORMAT, "'" + path + "' is not a whole number of 16-bit stereo frames");
    s->rate = 44100;
    s->channels = 2;
    s->bps = 16;
    s->total = n / 4;
    s->be = big_endian;
    return s;
}

// ------------------------------------------------------------------ FLAC file
class FlacFileSource : public PcmSource {
  public:
    File f;
    std::unique_ptr<SegmentDecoder> d;
    void seek(uint64_t frame) override {
        if (frame == d->next_) return;
        if (frame >= total) fail(FISO_E_ARG, "FLAC seek out of range");
        d->seek(frame);
    }
    size_t read(int32_t *out, size_t frames) override { return d->read(out, frames); }
};

std::unique_ptr<PcmSource> openFlacFile(const std::string &path) {
    auto s = std::make_unique<FlacFileSource>();
    s->f = File::open(path, "rb");
    s->d = std::make_unique<SegmentDecoder>(s->f, 0, s->f.size());
    s->rate = s->d->rate();
    s->channels = s->d->channels();
    s->bps = s->d->bps();
    s->total = s->d->totalSamples();
    if (s->total == 0) fail(FISO_E_UNSUPPORTED, "'" + path + "': FLAC file without total sample count");
    return s;
}

std::unique_ptr<PcmSource> openAnyAudio(const std::string &path) {
    uint8_t h[12] = {0};
    {
        File f = File::open(path, "rb");
        f.readSome(h, 12);
    }
    if (memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) return openWav(path);
    if (memcmp(h, "fLaC", 4) == 0 || memcmp(h, "ID3", 3) == 0) return openFlacFile(path);
    fail(FISO_E_UNSUPPORTED, "'" + path + "': unsupported audio file (WAV PCM and FLAC are accepted)");
}

}  // namespace fiso
