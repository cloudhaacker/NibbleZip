#include "arcfile.h"
#include <algorithm>
#include <cstdio>

namespace nz {

std::string ArcFile::partName(const std::string& base, uint64_t index) {
    char b[32]; snprintf(b, sizeof(b), ".%03llu", (unsigned long long)(index + 1));
    return base + b;
}
bool ArcFile::exists(const std::string& base) {
    plat::Stat st;
    return plat::stat(base, st) || plat::stat(partName(base, 0), st);
}
void ArcFile::removeAll(const std::string& base, uint64_t partSize, uint64_t totalSize) {
    if (!partSize) { plat::removeFile(base); return; }
    plat::Stat st;
    if (totalSize) {
        uint64_t n = (totalSize + partSize - 1) / partSize;
        for (uint64_t i = 0; i < n; i++) if (plat::stat(partName(base, i), st)) plat::removeFile(partName(base, i));
        return;
    }
    for (uint64_t i = 0; plat::stat(partName(base, i), st); i++) plat::removeFile(partName(base, i));
}
void ArcFile::detect(const std::string& path, std::string& base, uint64_t& partSize) {
    base = path; partSize = 0;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".001") == 0) {
        plat::Stat st;
        if (plat::stat(path, st) && !st.isDir && st.size > 0) { base = path.substr(0, path.size() - 4); partSize = st.size; }
    }
}
bool ArcFile::isPartOf(const std::string& k, const std::string& baseKey) {
    if (k.size() < baseKey.size() + 4 || k.compare(0, baseKey.size(), baseKey) != 0 || k[baseKey.size()] != '.') return false;
    for (size_t i = baseKey.size() + 1; i < k.size(); i++) if (k[i] < '0' || k[i] > '9') return false;
    return true;
}

bool ArcFile::create(const std::string& base, uint64_t partSize) {
    close();
    base_ = base; part_ = partSize; ro_ = false; total_ = 0;
    if (exists(base)) return false;
    std::unique_ptr<Slot> s(new Slot());
    if (!s->f.open(part_ ? partName(base, 0) : base, plat::OpenRWCreateNew)) return false;
    slots_.push_back(std::move(s));
    open_ = true;
    return true;
}
bool ArcFile::openExisting(const std::string& base, uint64_t partSize, bool readOnly) {
    close();
    base_ = base; part_ = partSize; ro_ = readOnly; total_ = 0;
    if (!part_) {
        std::unique_ptr<Slot> s(new Slot());
        if (!s->f.open(base, readOnly ? plat::OpenRead : plat::OpenRWExisting) || !s->f.size(total_)) return false;
        slots_.push_back(std::move(s));
        open_ = true;
        return true;
    }
    uint64_t n = 0; plat::Stat st; bool shortSeen = false;
    while (plat::stat(partName(base, n), st)) {
        if (shortSeen) return false;                 // a part in the middle is incomplete
        if (st.size > part_) return false;
        if (st.size < part_) shortSeen = true;
        total_ += st.size; n++;
    }
    if (n == 0) return false;
    open_ = true;
    return part(0, false) != nullptr;
}
bool ArcFile::totalSizeOf(const std::string& base, uint64_t partSize, uint64_t& total) {
    plat::Stat st; total = 0;
    if (!partSize) { if (!plat::stat(base, st)) return false; total = st.size; return true; }
    uint64_t n = 0;
    while (plat::stat(partName(base, n), st)) { total += st.size; n++; }
    return n > 0;
}
bool ArcFile::openForExtract(const std::string& base, uint64_t partSize, uint64_t totalSize, bool writable) {
    close();
    base_ = base; part_ = partSize; ro_ = !writable; total_ = totalSize; released_ = 0;
    open_ = true;
    if (!part_) {
        uint64_t sz = 0; plat::File* f = part(0, false);
        if (!f || !f->size(sz) || sz != totalSize) { close(); return false; }
        return true;
    }
    // Parts that were already consumed may be gone; the last part must still be there.
    plat::Stat st;
    uint64_t last = totalSize ? (totalSize - 1) / part_ : 0;
    if (!plat::stat(partName(base, last), st)) { close(); return false; }
    return true;
}
bool ArcFile::releaseBefore(uint64_t pos, uint64_t keepHead) {
    if (ro_ || !open_ || pos <= keepHead || pos <= released_) return true;
    const uint64_t mb = 1ull << 20;
    if (!part_) {
        plat::File* f = part(0, false);
        uint64_t from = std::max(keepHead, released_ & ~(mb - 1));
        if (!f || !f->punchHole(from, pos - from)) return false;
        released_ = pos;
        return true;
    }
    bool ok = true;
    uint64_t firstIdx = released_ / part_, curIdx = pos / part_;
    for (uint64_t i = firstIdx; i <= curIdx; i++) {
        uint64_t pStart = i * part_, pEnd = pStart + part_;
        if (i >= 1 && pEnd <= pos) {            // a whole part is no longer needed: delete it (part 1 is kept as the anchor)
            for (size_t k = 0; k < slots_.size(); k++) if (slots_[k]->idx == i) { slots_[k]->f.close(); slots_.erase(slots_.begin() + k); break; }
            plat::Stat st;
            if (plat::stat(partName(base_, i), st) && !plat::removeFile(partName(base_, i))) ok = false;
        } else {
            uint64_t from = i == 0 ? keepHead : 0, to = std::min(pos, pEnd) - pStart;
            plat::Stat st;
            if (to > from && plat::stat(partName(base_, i), st)) { plat::File* f = part(i, false); if (!f || !f->punchHole(from, to - from)) ok = false; }
        }
    }
    released_ = pos;
    return ok;
}
void ArcFile::close() {
    for (auto& s : slots_) { if (s->dirty) s->f.flush(); s->f.close(); }
    slots_.clear(); open_ = false;
}
plat::File* ArcFile::part(uint64_t idx, bool create) {
    for (size_t i = 0; i < slots_.size(); i++)
        if (slots_[i]->idx == idx) { if (i + 1 != slots_.size()) std::rotate(slots_.begin() + i, slots_.begin() + i + 1, slots_.end()); return &slots_.back()->f; }
    if (slots_.size() >= 8) {                        // keep only a few parts open at a time
        if (slots_.front()->dirty && !slots_.front()->f.flush()) return nullptr;
        slots_.erase(slots_.begin());
    }
    std::unique_ptr<Slot> s(new Slot()); s->idx = idx;
    std::string name = part_ ? partName(base_, idx) : base_;
    if (!s->f.open(name, ro_ ? plat::OpenRead : (create ? plat::OpenRWAlways : plat::OpenRWExisting))) return nullptr;
    slots_.push_back(std::move(s));
    return &slots_.back()->f;
}
bool ArcFile::readAt(uint64_t off, void* buf, size_t n, size_t& got) {
    got = 0;
    if (!part_) { plat::File* f = part(0, false); return f && f->readAt(off, buf, n, got); }
    uint8_t* p = (uint8_t*)buf;
    while (n > 0 && off < total_) {
        uint64_t idx = off / part_, in = off % part_;
        size_t chunk = (size_t)std::min<uint64_t>(n, part_ - in), g = 0;
        plat::File* f = part(idx, false);
        if (!f || !f->readAt(in, p, chunk, g)) return false;
        got += g; p += g; off += g; n -= g;
        if (g < chunk) break;
    }
    return true;
}
bool ArcFile::writeAt(uint64_t off, const void* buf, size_t n) {
    if (ro_) return false;
    if (!part_) {
        plat::File* f = part(0, false);
        if (!f || !f->writeAt(off, buf, n)) return false;
        slots_.back()->dirty = true; total_ = std::max(total_, off + n);
        return true;
    }
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        uint64_t idx = off / part_, in = off % part_;
        size_t chunk = (size_t)std::min<uint64_t>(n, part_ - in);
        plat::File* f = part(idx, true);
        if (!f || !f->writeAt(in, p, chunk)) return false;
        slots_.back()->dirty = true;
        p += chunk; off += chunk; n -= chunk;
        total_ = std::max(total_, off);
    }
    return true;
}
bool ArcFile::flush() {
    bool ok = true;
    for (auto& s : slots_) if (s->dirty) { if (s->f.flush()) s->dirty = false; else ok = false; }
    return ok;
}
bool ArcFile::truncate(uint64_t size) {
    if (ro_) return false;
    if (!part_) {
        plat::File* f = part(0, false);
        if (!f || !f->truncate(size)) return false;
        slots_.back()->dirty = true; total_ = size;
        return true;
    }
    uint64_t last = size == 0 ? 0 : (size - 1) / part_;
    plat::Stat st;
    for (uint64_t i = last + 1; plat::stat(partName(base_, i), st); i++) {   // remove parts that are no longer needed
        for (size_t k = 0; k < slots_.size(); k++) if (slots_[k]->idx == i) { slots_[k]->f.close(); slots_.erase(slots_.begin() + k); break; }
        if (!plat::removeFile(partName(base_, i))) return false;
    }
    plat::File* f = part(last, true);
    if (!f || !f->truncate(size - last * part_)) return false;
    slots_.back()->dirty = true; total_ = size;
    return true;
}

} // namespace nz
