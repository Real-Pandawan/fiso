// Container writer — SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include <cstring>

namespace fiso {

ContainerWriter::ContainerWriter(const std::string &target, bool overwrite) : target_(target), overwrite_(overwrite) {
    if (!overwrite && pathExists(target)) fail(FISO_E_EXISTS, "'" + target + "' already exists");
    tmp_ = tempPathFor(target);
    file_ = File::open(tmp_, "w+b");
    uint8_t z[kHeaderSize] = {0};
    file_.write(z, sizeof z);
}

ContainerWriter::~ContainerWriter() {
    if (!finished_) {
        try {
            file_.close();
        } catch (...) {
        }
        removeFile(tmp_);
    }
}

void ContainerWriter::align() {
    uint64_t end = file_.size();
    file_.seek(end);
    static const uint8_t z[8] = {0};
    if (end % 8) file_.write(z, 8 - end % 8);
}

size_t ContainerWriter::begin(const char *type, uint32_t flags) {
    if (open_chunk_) fail(FISO_E_INTERNAL, "nested chunk");
    align();
    cur_base_ = file_.tell();
    ChunkEntry e;
    e.type.assign(type, 4);
    e.flags = flags;
    e.offset = cur_base_;
    entries_.push_back(e);
    open_chunk_ = true;
    return entries_.size() - 1;
}

void ContainerWriter::end() {
    if (!open_chunk_) fail(FISO_E_INTERNAL, "no open chunk");
    uint64_t endpos = file_.size();
    auto &e = entries_.back();
    e.length = endpos - e.offset;
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    file_.seek(e.offset);
    uint64_t left = e.length;
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, buf.size());
        file_.read(buf.data(), n);
        h.update(buf.data(), n);
        left -= n;
    }
    e.sha = h.finish();
    file_.seek(endpos);
    open_chunk_ = false;
}

size_t ContainerWriter::addChunk(const char *type, uint32_t flags, const std::vector<uint8_t> &data) {
    size_t idx = begin(type, flags);
    file_.write(data.data(), data.size());
    auto &e = entries_.back();
    e.length = data.size();
    Sha256 h;
    h.update(data.data(), data.size());
    e.sha = h.finish();
    open_chunk_ = false;
    return idx;
}

size_t ContainerWriter::copyChunk(Image &src, size_t idx, const Progress &prog) {
    const ChunkEntry &c = src.chunks.at(idx);
    size_t out = begin(c.type.c_str(), c.flags);
    std::vector<uint8_t> buf(1 << 20);
    Sha256 h;
    uint64_t left = c.length, done = 0;
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, buf.size());
        src.file().seek(c.offset + done);
        src.file().read(buf.data(), n);
        h.update(buf.data(), n);
        file_.write(buf.data(), n);
        left -= n;
        done += n;
        prog("copy", done, c.length);
    }
    auto &e = entries_.back();
    e.length = c.length;
    e.sha = h.finish();
    e.version = c.version;
    if (e.sha != c.sha) fail(FISO_E_INTEGRITY, "source chunk " + c.type + " is corrupted (SHA-256 mismatch)");
    open_chunk_ = false;
    return out;
}

void ContainerWriter::finish() {
    if (open_chunk_) fail(FISO_E_INTERNAL, "chunk left open");
    align();
    uint64_t toff = file_.tell();
    std::vector<uint8_t> t;
    for (auto &e : entries_) {
        putstr(t, e.type, 4);
        put32(t, e.flags);
        put64(t, e.offset);
        put64(t, e.length);
        t.insert(t.end(), e.sha.begin(), e.sha.end());
        put32(t, e.version);
        put32(t, 0);
    }
    file_.write(t.data(), t.size());
    Sha256 th;
    th.update(t.data(), t.size());
    Hash tsha = th.finish();
    final_size_ = toff + t.size();

    std::vector<uint8_t> h(kMagic, kMagic + 8);
    put16(h, FISO_FORMAT_VERSION_MAJOR);
    put16(h, FISO_FORMAT_VERSION_MINOR);
    put32(h, kHeaderSize);
    put32(h, 0);
    put32(h, (uint32_t)entries_.size());
    put64(h, toff);
    put64(h, final_size_);
    h.insert(h.end(), tsha.begin(), tsha.end());
    putz(h, 20);
    put32(h, crc32(h.data(), h.size()));
    file_.seek(0);
    file_.write(h.data(), h.size());
    file_.sync();
    file_.close();
    renameReplace(tmp_, target_, overwrite_);
    finished_ = true;
}

}  // namespace fiso
