// Image reader — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstring>

namespace fiso {

// ------------------------------------------------------------------ SegmentDecoder
static FLAC__StreamDecoderReadStatus dRead(const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *u) {
    auto *d = (SegmentDecoder *)u;
    uint64_t left = d->len_ - d->pos_;
    size_t n = (size_t)std::min<uint64_t>(*bytes, left);
    if (n == 0) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    try {
        d->f_.seek(d->base_ + d->pos_);
        n = d->f_.readSome(buffer, n);
    } catch (...) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    if (n == 0) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    d->pos_ += n;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderSeekStatus dSeek(const FLAC__StreamDecoder *, FLAC__uint64 off, void *u) {
    auto *d = (SegmentDecoder *)u;
    if (off > d->len_) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    d->pos_ = off;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}
static FLAC__StreamDecoderTellStatus dTell(const FLAC__StreamDecoder *, FLAC__uint64 *off, void *u) {
    *off = ((SegmentDecoder *)u)->pos_;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderLengthStatus dLen(const FLAC__StreamDecoder *, FLAC__uint64 *len, void *u) {
    *len = ((SegmentDecoder *)u)->len_;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}
static FLAC__bool dEof(const FLAC__StreamDecoder *, void *u) {
    auto *d = (SegmentDecoder *)u;
    return d->pos_ >= d->len_;
}
static FLAC__StreamDecoderWriteStatus dWrite(const FLAC__StreamDecoder *, const FLAC__Frame *fr, const FLAC__int32 *const buf[],
                                             void *u) {
    auto *d = (SegmentDecoder *)u;
    uint32_t ch = fr->header.channels, n = fr->header.blocksize;
    if (ch != d->ch_ || fr->header.bits_per_sample != d->bps_) {
        d->error_ = true;
        d->errmsg_ = "frame format differs from STREAMINFO";
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    size_t base = d->buf_.size();
    d->buf_.resize(base + (size_t)n * ch);
    int32_t *o = d->buf_.data() + base;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < ch; c++) *o++ = buf[c][i];
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void dMeta(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *m, void *u) {
    auto *d = (SegmentDecoder *)u;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
        d->rate_ = m->data.stream_info.sample_rate;
        d->ch_ = m->data.stream_info.channels;
        d->bps_ = m->data.stream_info.bits_per_sample;
        d->total_ = m->data.stream_info.total_samples;
        memcpy(d->md5_, m->data.stream_info.md5sum, 16);
    }
}
static void dErr(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus st, void *u) {
    auto *d = (SegmentDecoder *)u;
    d->error_ = true;
    d->errmsg_ = FLAC__StreamDecoderErrorStatusString[st];
}

SegmentDecoder::SegmentDecoder(File &f, uint64_t base, uint64_t len) : f_(f), base_(base), len_(len) {
    auto *dec = FLAC__stream_decoder_new();
    if (!dec) fail(FISO_E_INTERNAL, "out of memory");
    dec_ = dec;
    FLAC__stream_decoder_set_md5_checking(dec, true);
    if (FLAC__stream_decoder_init_stream(dec, dRead, dSeek, dTell, dLen, dEof, dWrite, dMeta, dErr, this) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        fail(FISO_E_FLAC, "cannot initialise FLAC decoder");
    if (!FLAC__stream_decoder_process_until_end_of_metadata(dec) || error_ || rate_ == 0)
        fail(FISO_E_INTEGRITY, "invalid FLAC stream header" + (errmsg_.empty() ? "" : ": " + errmsg_));
}

SegmentDecoder::~SegmentDecoder() {
    if (dec_) {
        FLAC__stream_decoder_finish((FLAC__StreamDecoder *)dec_);
        FLAC__stream_decoder_delete((FLAC__StreamDecoder *)dec_);
    }
}

bool SegmentDecoder::fill() {
    auto *dec = (FLAC__StreamDecoder *)dec_;
    while (bufpos_ >= buf_.size()) {
        buf_.clear();
        bufpos_ = 0;
        if (FLAC__stream_decoder_get_state(dec) == FLAC__STREAM_DECODER_END_OF_STREAM) return false;
        if (!FLAC__stream_decoder_process_single(dec) || error_)
            fail(FISO_E_INTEGRITY, "FLAC decoding error" + (errmsg_.empty() ? "" : ": " + errmsg_));
    }
    return true;
}

size_t SegmentDecoder::read(int32_t *out, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (bufpos_ >= buf_.size() && !fill()) break;
        size_t avail = (buf_.size() - bufpos_) / ch_;
        size_t n = std::min(avail, frames - done);
        memcpy(out + done * ch_, buf_.data() + bufpos_, n * ch_ * sizeof(int32_t));
        bufpos_ += n * ch_;
        done += n;
    }
    next_ += done;
    return done;
}

void SegmentDecoder::seek(uint64_t sample) {
    auto *dec = (FLAC__StreamDecoder *)dec_;
    buf_.clear();
    bufpos_ = 0;
    if (sample >= total_) fail(FISO_E_ARG, "seek beyond end of segment");
    if (!FLAC__stream_decoder_seek_absolute(dec, sample)) {
        FLAC__stream_decoder_flush(dec);
        buf_.clear();
        if (!FLAC__stream_decoder_seek_absolute(dec, sample)) fail(FISO_E_INTEGRITY, "FLAC seek failed");
    }
    error_ = false;
    next_ = sample;
}

bool SegmentDecoder::md5Ok() {
    auto *dec = (FLAC__StreamDecoder *)dec_;
    bool ok = FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    dec_ = nullptr;
    return ok;
}

// ------------------------------------------------------------------ Image
Image::~Image() = default;

std::vector<uint8_t> Image::readChunk(size_t idx, uint64_t max) {
    auto &c = chunks.at(idx);
    if (c.length > max) fail(FISO_E_FORMAT, c.type + " chunk too large");
    std::vector<uint8_t> v((size_t)c.length);
    file_.seek(c.offset);
    file_.read(v.data(), v.size());
    Sha256 h;
    h.update(v.data(), v.size());
    if (h.finish() != c.sha) fail(FISO_E_INTEGRITY, c.type + " chunk SHA-256 mismatch");
    return v;
}

Picture Image::readPicture(size_t ci) {
    auto v = readChunk(ci, kMaxPict);
    return parsePicture(v.data(), v.size());
}

const SegmentRec *Image::segmentForTrack(uint8_t number) const {
    for (auto &s : layout.segments)
        if (s.kind == 1 && s.track == number) return &s;
    return nullptr;
}

std::unique_ptr<Image> Image::open(const std::string &path) {
    std::unique_ptr<Image> img(new Image());
    img->path = path;
    img->file_ = File::open(path, "rb");
    File &f = img->file_;
    uint64_t real = f.size();
    if (real < kHeaderSize + kEntrySize) fail(FISO_E_FORMAT, "file too small to be a FISO image");
    uint8_t h[kHeaderSize];
    f.seek(0);
    f.read(h, sizeof h);
    if (memcmp(h, kMagic, 8) != 0) fail(FISO_E_FORMAT, "not a FISO image (bad signature)");
    img->vmajor = get16(h + 8);
    img->vminor = get16(h + 10);
    if (img->vmajor != FISO_FORMAT_VERSION_MAJOR)
        fail(FISO_E_UNSUPPORTED, "unsupported FISO major version " + std::to_string(img->vmajor));
    if (crc32(h, 92) != get32(h + 92)) fail(FISO_E_FORMAT, "header CRC mismatch");
    if (get32(h + 12) != kHeaderSize) fail(FISO_E_FORMAT, "invalid header size");
    if (get32(h + 16) != 0) img->warnings.push_back("non-zero file flags ignored");
    uint32_t count = get32(h + 20);
    uint64_t toff = get64(h + 24);
    img->file_size = get64(h + 32);
    if (count < 1 || count > kMaxChunks) fail(FISO_E_FORMAT, "invalid chunk count");
    if (toff % 8 || toff < kHeaderSize || toff > real) fail(FISO_E_FORMAT, "invalid chunk table offset");
    if (img->file_size != real) fail(FISO_E_FORMAT, "file size mismatch (truncated or extended file)");
    if ((real - toff) != (uint64_t)count * kEntrySize) fail(FISO_E_FORMAT, "chunk table size mismatch");

    std::vector<uint8_t> table((size_t)count * kEntrySize);
    f.seek(toff);
    f.read(table.data(), table.size());
    Sha256 th;
    th.update(table.data(), table.size());
    if (memcmp(th.finish().data(), h + 40, 32) != 0) fail(FISO_E_INTEGRITY, "chunk table SHA-256 mismatch");

    int nl = 0, nm = 0, ncd = 0, nlog = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = table.data() + (size_t)i * kEntrySize;
        ChunkEntry c;
        c.type.assign((const char *)e, 4);
        c.flags = get32(e + 4);
        c.offset = get64(e + 8);
        c.length = get64(e + 16);
        memcpy(c.sha.data(), e + 24, 32);
        c.version = get32(e + 56);
        if (c.offset % 8 || c.offset < kHeaderSize || c.offset > toff || c.length > toff - c.offset)
            fail(FISO_E_FORMAT, "chunk " + std::to_string(i) + " out of bounds");
        if (c.type == "LAYT")
            nl++;
        else if (c.type == "META")
            nm++;
        else if (c.type == "PICT")
            img->pict_chunks.push_back(i);
        else if (c.type == "CDTX")
            ncd++;
        else if (c.type == "RLOG")
            nlog++;
        else if (c.type != "FLAC") {
            if (c.flags & kChunkCritical) fail(FISO_E_UNSUPPORTED, "unknown critical chunk '" + c.type + "'");
            img->warnings.push_back("unknown chunk '" + c.type + "' ignored");
        }
        img->chunks.push_back(c);
    }
    if (nl != 1 || nm != 1) fail(FISO_E_FORMAT, "exactly one LAYT and one META chunk are required");
    if (ncd > 1 || nlog > 1) fail(FISO_E_FORMAT, "duplicate CDTX or RLOG chunk");
    // overlap check
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    for (auto &c : img->chunks) spans.push_back({c.offset, c.length});
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); i++)
        if (spans[i - 1].first + spans[i - 1].second > spans[i].first) fail(FISO_E_FORMAT, "overlapping chunks");

    size_t flac_count = 0;
    for (size_t i = 0; i < img->chunks.size(); i++) {
        auto &c = img->chunks[i];
        if (c.type == "FLAC") flac_count++;
        if (c.type == "LAYT") {
            auto v = img->readChunk(i, 1 << 20);
            img->layout = parseLayout(v.data(), v.size());
        }
    }
    for (auto &s : img->layout.segments) {
        if (s.chunk_index >= img->chunks.size() || img->chunks[s.chunk_index].type != "FLAC")
            fail(FISO_E_FORMAT, "segment references a non-FLAC chunk");
    }
    if (flac_count != img->layout.segments.size()) fail(FISO_E_FORMAT, "unreferenced FLAC chunk present");
    for (size_t i = 0; i < img->chunks.size(); i++) {
        if (img->chunks[i].type != "META") continue;
        auto v = img->readChunk(i, kMaxMeta);
        img->meta_raw.assign(v.begin(), v.end());
        img->meta = json::parse(img->meta_raw, nullptr, false);
        if (img->meta.is_discarded()) fail(FISO_E_FORMAT, "META is not valid JSON");
        validateMeta(img->meta, img->layout);
    }
    img->decoders_.resize(img->layout.segments.size());
    return img;
}

size_t Image::readPcm(uint64_t start, size_t frames, int32_t *out) {
    auto &segs = layout.segments;
    if (start >= layout.total) return 0;
    frames = (size_t)std::min<uint64_t>(frames, layout.total - start);
    size_t done = 0;
    // binary search first segment
    size_t si = std::upper_bound(segs.begin(), segs.end(), start, [](uint64_t v, const SegmentRec &s) { return v < s.start; }) -
                segs.begin() - 1;
    while (done < frames && si < segs.size()) {
        auto &s = segs[si];
        uint64_t local = start + done - s.start;
        size_t want = (size_t)std::min<uint64_t>(frames - done, s.length - local);
        auto &dp = decoders_[si];
        auto &c = chunks[s.chunk_index];
        if (!dp) {
            dp = std::make_unique<SegmentDecoder>(file_, c.offset, c.length);
            if (dp->rate_ != layout.rate || dp->ch_ != layout.channels || dp->bps_ != layout.bps)
                fail(FISO_E_FORMAT, "FLAC segment format differs from LAYT");
            if (dp->total_ != s.length) fail(FISO_E_FORMAT, "FLAC segment length differs from LAYT");
            if (local) dp->seek(local);
        } else if (dp->next_ != local) {
            dp->seek(local);
        }
        size_t got = dp->read(out + done * layout.channels, want);
        if (got != want) fail(FISO_E_INTEGRITY, "FLAC segment shorter than declared");
        done += got;
        si++;
    }
    return done;
}

}  // namespace fiso
