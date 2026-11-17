// Extraction that gives the archive's disk space back as it goes.
//
// The archive is read front to back. At safe restart points (a 7z block that starts with a dictionary
// reset, a Deflate block boundary, the end of a file or folder) the extractor
//   1. forces the files written so far to disk, reads them back and compares checksums,
//   2. records the position in a journal,
//   3. releases the part of the archive that has now been consumed.
// A crash at any moment leaves a state that the next run continues from.
#include "engine.h"
#include "platform.h"
#include "arcfile.h"
#include "common.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#include "7z.h"
#include "Alloc.h"
#include "Bra.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"
#include <zlib.h>

namespace nz {
using namespace detail;
namespace {

const uint32_t XMAGIC = 0x31585A4E;   // "NZX1"
enum { X_HEADER = 1, X_COMMIT = 2 };
const char* const XEXT = ".nzx";
const uint64_t KEEP_HEAD = 64;        // the first bytes of the archive (its signature) are never released

struct XHeader { std::string archive, dest; uint64_t partSize = 0, totalSize = 0; bool eat = true; };
// Where to continue from.
struct XState {
    uint64_t unit = 0;       // 7z: folder number. ZIP: entry number (in file order)
    bool mid = false;        // stopped inside that unit
    uint64_t inOff = 0;      // 7z: offset inside the folder's packed data. ZIP: archive offset of the next input byte
    uint64_t outOff = 0;     // 7z: bytes of the folder produced so far. ZIP: bytes of the entry written so far
    uint32_t bits = 0;       // ZIP Deflate: bits of the previous byte still to be used
    uint64_t fileOrd = 0;    // 7z: which file of the folder is current
    uint64_t fileOff = 0;    // 7z: offset inside that file
    uint32_t crcState = CRC_INIT_VAL;
    std::vector<uint8_t> window;   // ZIP Deflate: the last 32 KB of output
    uint64_t doneOut = 0, filesDone = 0;
};

static void putState(Buf& b, const XState& s) {
    b.u8(X_COMMIT); b.u64(s.unit); b.u8(s.mid ? 1 : 0); b.u64(s.inOff); b.u64(s.outOff); b.u32(s.bits); b.u64(s.fileOrd); b.u64(s.fileOff);
    b.u32(s.crcState); b.u32((uint32_t)s.window.size()); b.bytes(s.window.data(), s.window.size()); b.u64(s.doneOut); b.u64(s.filesDone);
}
static size_t parseX(const std::vector<uint8_t>& b, XHeader& h, XState& st) {
    h = XHeader(); st = XState();
    size_t pos = 0; bool have = false;
    while (pos + 8 <= b.size()) {
        Rd fr(b.data() + pos, 8);
        uint32_t len = fr.u32(), crc = fr.u32();
        if (len == 0 || len > (1u << 26) || pos + 8 + len > b.size()) break;
        const uint8_t* pl = b.data() + pos + 8;
        if (CrcCalc(pl, len) != crc) break;
        Rd r(pl, len);
        uint8_t type = r.u8();
        if (!have) {
            if (type != X_HEADER || r.u32() != XMAGIC) break;
            h.archive = r.str(); h.dest = r.str(); h.partSize = r.u64(); h.totalSize = r.u64(); h.eat = r.u8() != 0;
            if (r.bad) break;
            have = true;
        } else if (type == X_COMMIT) {
            XState s; s.unit = r.u64(); s.mid = r.u8() != 0; s.inOff = r.u64(); s.outOff = r.u64(); s.bits = r.u32(); s.fileOrd = r.u64(); s.fileOff = r.u64();
            s.crcState = r.u32(); uint32_t wl = r.u32();
            if (r.bad || wl > 65536 || r.pos + wl > r.n) break;
            s.window.assign(pl + r.pos, pl + r.pos + wl); r.pos += wl;
            s.doneOut = r.u64(); s.filesDone = r.u64();
            if (r.bad) break;
            st = std::move(s);
        } else break;
        pos += 8 + len;
    }
    return have ? pos : 0;
}
static bool loadX(const std::string& archive, XHeader& h, XState& st, std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> ba, bb; XHeader ha, hb; XState sa, sb; size_t la = 0, lb = 0;
    if (readAll(archive + XEXT, ba)) la = parseX(ba, ha, sa);
    if (readAll(mirrorPathFor(archive, XEXT), bb)) { lb = parseX(bb, hb, sb); if (lb && !plat::samePath(hb.archive, archive)) lb = 0; }
    if (!la && !lb) return false;
    if (la >= lb) { h = ha; st = std::move(sa); ba.resize(la); bytes = std::move(ba); }
    else { h = hb; st = std::move(sb); bb.resize(lb); bytes = std::move(bb); }
    return true;
}

// ---------------------------------------------------------------- names
// Turns a name from an archive into a safe relative path: no drive letters, no "..", no absolute paths,
// nothing Windows cannot store. Extracted files can therefore never land outside the destination folder.
static std::string safeRel(const std::string& name) {
    std::vector<std::string> parts; std::string cur;
    auto flush = [&]() {
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '.')) cur.pop_back();
        if (!cur.empty()) {
            std::string up; for (char c : cur) up += (char)toupper((unsigned char)c);
            size_t dot = up.find('.'); std::string stem = dot == std::string::npos ? up : up.substr(0, dot);
            static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                             "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
            for (auto r : reserved) if (stem == r) { cur = "_" + cur; break; }
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (c == '/' || c == '\\') { if (cur == "..") cur = "_"; flush(); continue; }
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') cur += '_';
        else cur += ch;
    }
    if (cur == "..") cur = "_";
    flush();
    std::string out;
    for (auto& p : parts) { if (p == ".") continue; if (!out.empty()) out += '/'; out += p; }
    return out.empty() ? "_unnamed" : out;
}
static std::string lowerAscii(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

struct Item {
    std::string rel, path;
    uint64_t size = 0, mtime = 0;
    uint32_t crc = 0;
    bool hasCrc = false, isDir = false, hasMtime = false;
};
// Gives every item a unique, safe path under dest.
static void assignPaths(std::vector<Item>& items, const std::string& dest) {
    // If everything sits in one top folder with the same name as the destination ("Game.7z" holding "Game\..."),
    // drop that level, so the result is Game\... and not Game\Game\...
    std::string root; bool single = !items.empty();
    for (auto& it : items) {
        std::string r = safeRel(it.rel); size_t sl = r.find('/');
        std::string top = sl == std::string::npos ? r : r.substr(0, sl);
        if (sl == std::string::npos && !it.isDir) { single = false; break; }
        if (root.empty()) root = top; else if (root != top) { single = false; break; }
    }
    std::string dn = lowerAscii(plat::baseName(dest)), rn = lowerAscii(root);
    bool strip = single && !root.empty() && (dn == rn || (dn.size() > rn.size() + 2 && dn.compare(0, rn.size() + 2, rn + " (") == 0));
    std::set<std::string> used;
    for (auto& it : items) {
        std::string rel = safeRel(it.rel);
        if (strip) { if (rel == root) { it.path = dest; it.rel = ""; continue; } rel = rel.substr(root.size() + 1); }
        std::string base = rel; int n = 2;
        while (!used.insert(lowerAscii(rel)).second) {
            if (it.isDir) break;   // the same folder listed twice is harmless
            size_t slash = base.find_last_of('/'), dot = base.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == 0) rel = base + " (" + std::to_string(n) + ")";
            else rel = base.substr(0, dot) + " (" + std::to_string(n) + ")" + base.substr(dot);
            n++;
        }
        it.rel = rel;
        std::string native = rel;
        for (auto& c : native) if (c == '/') c = plat::SEP;
        it.path = plat::join(dest, native);
    }
}

static const uint16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};
static void putUtf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
}
static bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i]; int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
        for (int k = 1; k <= n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}
static std::string utf8From16(const uint16_t* s, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
        putUtf8(o, c);
    }
    return o;
}

// ---------------------------------------------------------------- context and output
struct XCtx {
    ExtractOptions opt; Callbacks* cb = nullptr; ExtractResult res;
    ArcFile arc; Journal j; XHeader h; XState st; Progress pr;
    bool dry = false;              // checking pass: decompress and compare checksums, write nothing
    uint64_t interval = 64ull << 20, sinceCommit = 0, sinceSpaceCheck = 0, baseDone = 0;
    int stop = 0; bool warnedNoRelease = false;
    std::chrono::steady_clock::time_point lastTick;
    // current output file
    const Item* cur = nullptr; plat::File f; uint64_t off = 0; uint32_t crc = CRC_INIT_VAL;
    // what has been written since the last commit, for the read-back check
    struct Range { std::string path; uint64_t off, len; };
    std::vector<Range> ranges; uint32_t segCrc = CRC_INIT_VAL;

    bool fail(const std::string& m, bool resumable = true) { if (res.error.empty()) { res.error = m; res.resumable = resumable; } return false; }
    void warn(const std::string& w) { if (res.warnings.size() < 200) res.warnings.push_back(w); }
    void tick(bool force) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - lastTick < std::chrono::milliseconds(120)) return;
        lastTick = now;
        pr.doneBytes = baseDone + st.doneOut; pr.filesDone = st.filesDone; pr.phase = dry ? 1 : 2;
        cb->progress(pr);
    }
    bool poll() { if (cb->poll() == 1) { stop = 1; return false; } return true; }

    bool begin(const Item& it, uint64_t resumeOff, uint32_t resumeCrc) {
        cur = &it; off = resumeOff; crc = resumeCrc; pr.current = it.rel;
        if (dry) return true;
        plat::makeDirs(plat::parentDir(it.path));
        if (!f.open(it.path, plat::OpenRWAlways)) return fail("Cannot create " + it.path + ": " + plat::lastError());
        if (!f.truncate(resumeOff)) return fail("Cannot write " + it.path + ": " + plat::lastError());
        return true;
    }
    bool write(const uint8_t* p, size_t n) {
        if (!cur) return fail("The archive holds more data than its index lists.", false);
        if (off + n > cur->size) return fail("The archive holds more data for " + cur->rel + " than its index lists.", false);
        crc = CrcUpdate(crc, p, n);
        if (!dry) {
            sinceSpaceCheck += n;
            if (sinceSpaceCheck >= (16ull << 20)) {
                sinceSpaceCheck = 0; uint64_t fr = 0;
                if (plat::freeSpace(h.dest, fr) && fr < (8ull << 20)) return fail("Not enough free space to continue extracting (" + humanSize(fr) + " free). Free some space and resume.");
            }
            if (!f.writeAt(off, p, n)) return fail("Writing " + cur->path + " failed: " + plat::lastError());
            segCrc = CrcUpdate(segCrc, p, n);
            if (!ranges.empty() && ranges.back().path == cur->path && ranges.back().off + ranges.back().len == off) ranges.back().len += n;
            else { Range r; r.path = cur->path; r.off = off; r.len = n; ranges.push_back(r); }
        }
        off += n; st.doneOut += n; sinceCommit += n;
        return true;
    }
    bool end() {
        if (!cur) return true;
        const Item& it = *cur;
        if (off != it.size) return fail("The data for " + it.rel + " ends early.", false);
        if (it.hasCrc && CRC_GET_DIGEST(crc) != it.crc) return fail("Checksum mismatch in " + it.rel + ": the archive is damaged.", false);
        if (!dry) {
            if (!f.flush()) return fail("Could not flush " + it.path + ": " + plat::lastError());
            f.close();
            if (it.hasMtime) plat::setMTime(it.path, it.mtime);
        }
        cur = nullptr; st.filesDone++;
        return true;
    }
    // Forces everything written since the last commit to disk, reads it back and compares.
    bool flushVerify() {
        if (dry) return true;
        if (f.isOpen() && !f.flush()) return fail("Could not flush " + cur->path + ": " + plat::lastError());
        uint32_t c2 = CRC_INIT_VAL; std::vector<uint8_t> tmp(1 << 20);
        for (auto& r : ranges) {
            plat::File rf; plat::File* src = &rf;
            if (cur && f.isOpen() && r.path == cur->path) src = &f;
            else if (!rf.open(r.path, plat::OpenRead)) return fail("Cannot re-read " + r.path + ": " + plat::lastError());
            uint64_t o = r.off, left = r.len;
            while (left) {
                size_t want = (size_t)std::min<uint64_t>(left, tmp.size()), got = 0;
                if (!src->readAt(o, tmp.data(), want, got) || got != want) return fail("Cannot re-read " + r.path + ": " + plat::lastError());
                c2 = CrcUpdate(c2, tmp.data(), got); o += got; left -= got;
            }
        }
        if (c2 != segCrc) return fail("Verification failed: extracted data did not read back from disk correctly. The archive was not shrunk any further.");
        ranges.clear(); segCrc = CRC_INIT_VAL;
        return true;
    }
    // A safe restart point: verify, record, then release the consumed part of the archive.
    bool commit(XState next, uint64_t releaseUpTo) {
        if (dry) return true;
        if (!flushVerify()) return false;
        CP("x-verified");
        next.doneOut = st.doneOut; next.filesDone = st.filesDone;
        Buf rec; putState(rec, next);
        if (!j.append(rec)) return fail("Could not write the journal: " + plat::lastError());
        CP("x-journaled");
        st = std::move(next);
        if (h.eat) {
            if (!arc.releaseBefore(releaseUpTo, KEEP_HEAD) && !warnedNoRelease) {
                warnedNoRelease = true;
                warn("This drive cannot release space inside the archive while it is being read, so its space is freed when the extraction finishes.");
            }
            pr.reclaimedBytes = std::max(pr.reclaimedBytes, releaseUpTo);
            pr.packedBytes = h.totalSize > releaseUpTo ? h.totalSize - releaseUpTo : 0;
        }
        CP("x-released");
        sinceCommit = 0;
        tick(true);
        return true;
    }
};

// Buffered forward reader over one range of the archive.
struct PackReader {
    ArcFile* a = nullptr; uint64_t pos = 0, end = 0; std::vector<uint8_t> buf; size_t b0 = 0, b1 = 0;
    void init(ArcFile* arc, uint64_t from, uint64_t to) { a = arc; pos = from; end = to; buf.resize(1 << 20); b0 = b1 = 0; }
    uint64_t at() const { return pos - (b1 - b0); }          // archive offset of the next unread byte
    size_t avail() const { return b1 - b0; }
    const uint8_t* ptr() const { return buf.data() + b0; }
    void advance(size_t n) { b0 += n; }
    // Makes at least n bytes available (n must fit in the buffer). False at end of data or on a read error.
    bool need(size_t n) {
        if (avail() >= n) return true;
        if (n > buf.size()) return false;
        memmove(buf.data(), buf.data() + b0, avail()); b1 -= b0; b0 = 0;
        while (avail() < n && pos < end) {
            size_t want = (size_t)std::min<uint64_t>(buf.size() - b1, end - pos), got = 0;
            if (!a->readAt(pos, buf.data() + b1, want, got) || got == 0) return false;
            b1 += got; pos += got;
        }
        return avail() >= n;
    }
};

// ---------------------------------------------------------------- 7z
struct SeekIn { ISeekInStream vt; ArcFile* f; uint64_t pos; uint64_t size; };
static SRes siRead(ISeekInStreamPtr pp, void* buf, size_t* size) {
    SeekIn* s = Z7_CONTAINER_FROM_VTBL(pp, SeekIn, vt);
    size_t got = 0;
    if (*size && !s->f->readAt(s->pos, buf, *size, got)) return SZ_ERROR_READ;
    s->pos += got; *size = got;
    return SZ_OK;
}
static SRes siSeek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin) {
    SeekIn* s = Z7_CONTAINER_FROM_VTBL(pp, SeekIn, vt);
    Int64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? (Int64)s->pos : (Int64)s->size;
    Int64 np = base + *pos;
    if (np < 0) return SZ_ERROR_READ;
    s->pos = (uint64_t)np; *pos = np;
    return SZ_OK;
}

enum { M_COPY = 0, M_LZMA = 1, M_LZMA2 = 2 };
struct FolderPlan {
    uint64_t packPos = 0, packSize = 0, unpackSize = 0;
    int method = M_COPY; bool bcj = false;
    std::vector<uint8_t> props;
    std::vector<size_t> files;     // indexes into the item list, in order
};

static bool decodeFolder(XCtx& c, const FolderPlan& fp, uint64_t fi, bool last, const std::vector<Item>& items) {
    bool resume = c.st.mid && c.st.unit == fi;
    uint64_t inOff = resume ? c.st.inOff : 0, produced = resume ? c.st.outOff : 0;
    size_t ord = resume ? (size_t)c.st.fileOrd : 0;
    if (resume && c.st.fileOff > 0) {
        if (ord >= fp.files.size()) return c.fail("The journal does not match this archive.", false);
        if (!c.begin(items[fp.files[ord]], c.st.fileOff, c.st.crcState)) return false;
    }
    PackReader rd; rd.init(&c.arc, fp.packPos + inOff, fp.packPos + fp.packSize);
    // Hands decompressed bytes to the files of this folder, in order.
    auto emit = [&](const uint8_t* p, size_t n) -> bool {
        produced += n;
        while (n) {
            if (!c.cur) {
                while (ord < fp.files.size() && items[fp.files[ord]].size == 0) { if (!c.begin(items[fp.files[ord]], 0, CRC_INIT_VAL) || !c.end()) return false; ord++; }
                if (ord >= fp.files.size()) return c.fail("The archive holds more data than its index lists.", false);
                if (!c.begin(items[fp.files[ord]], 0, CRC_INIT_VAL)) return false;
            }
            size_t t = (size_t)std::min<uint64_t>(n, c.cur->size - c.off);
            if (!c.write(p, t)) return false;
            p += t; n -= t;
            if (c.off == c.cur->size) { if (!c.end()) return false; ord++; }
        }
        return true;
    };
    // Optional x86 branch filter between the decompressor and the files.
    std::vector<uint8_t> fb; UInt32 pc = 0, bst = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    auto out = [&](const uint8_t* p, size_t n) -> bool {
        if (!fp.bcj) return emit(p, n);
        fb.insert(fb.end(), p, p + n);
        Byte* e = z7_BranchConvSt_X86_Dec(fb.data(), fb.size(), pc, &bst);
        size_t done = (size_t)(e - fb.data());
        if (done && !emit(fb.data(), done)) return false;
        pc += (UInt32)done; fb.erase(fb.begin(), fb.begin() + done);
        return true;
    };
    std::vector<uint8_t> ob(1 << 20);
    bool ok = true;
    if (fp.method == M_COPY) {
        while (ok && produced < fp.unpackSize) {
            if (!rd.need(1)) { ok = c.fail("The archive data ends early or cannot be read: " + plat::lastError()); break; }
            size_t n = rd.avail();
            ok = out(rd.ptr(), n); rd.advance(n);
            if (ok && !fp.bcj && !c.dry && c.sinceCommit >= c.interval && produced < fp.unpackSize) {
                XState s; s.unit = fi; s.mid = true; s.inOff = rd.at() - fp.packPos; s.outOff = produced; s.fileOrd = ord;
                s.fileOff = c.cur ? c.off : 0; s.crcState = c.cur ? c.crc : CRC_INIT_VAL;
                ok = c.commit(s, rd.at());
            }
            c.tick(false);
            if (ok && !c.poll()) ok = false;
        }
    } else if (fp.method == M_LZMA2) {
        CLzma2Dec dec; Lzma2Dec_CONSTRUCT(&dec);
        if (fp.props.size() != 1 || Lzma2Dec_Allocate(&dec, fp.props[0], &g_Alloc) != SZ_OK) return c.fail("Not enough memory to decompress this archive (it needs a very large dictionary).", false);
        Lzma2Dec_Init(&dec);
        bool first = true, finished = false;
        while (ok && !finished) {
            if (!rd.need(1)) { ok = c.fail("The archive data ends early or cannot be read."); break; }
            uint8_t c0 = rd.ptr()[0]; size_t chunk = 1;
            if (c0 != 0) {
                size_t hl = c0 >= 0x80 ? (c0 >= 0xC0 ? 6 : 5) : 3;
                if ((c0 < 0x80 && c0 > 2) || !rd.need(hl)) { ok = c.fail("The archive data is damaged.", false); break; }
                const uint8_t* q = rd.ptr();
                chunk = c0 >= 0x80 ? hl + (((size_t)q[3] << 8) | q[4]) + 1 : 3 + (((size_t)q[1] << 8) | q[2]) + 1;
                bool reset = c0 == 1 || c0 >= 0xE0;
                // A block that starts with a dictionary reset needs nothing that came before it: a safe restart point.
                if (reset && !first && !fp.bcj && !c.dry && c.sinceCommit >= c.interval) {
                    XState s; s.unit = fi; s.mid = true; s.inOff = rd.at() - fp.packPos; s.outOff = produced; s.fileOrd = ord;
                    s.fileOff = c.cur ? c.off : 0; s.crcState = c.cur ? c.crc : CRC_INIT_VAL;
                    if (!c.commit(s, rd.at())) { ok = false; break; }
                    Lzma2Dec_Init(&dec);   // continue exactly as a resumed run would
                }
                if (!rd.need(chunk)) { ok = c.fail("The archive data ends early or cannot be read."); break; }
            }
            size_t sp = 0;
            for (;;) {
                SizeT dl = ob.size(), sl = chunk - sp; ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
                SRes r = Lzma2Dec_DecodeToBuf(&dec, ob.data(), &dl, rd.ptr() + sp, &sl, LZMA_FINISH_ANY, &status);
                sp += sl;
                if (r != SZ_OK) { ok = c.fail("The archive data is damaged near " + c.pr.current + ".", false); break; }
                if (dl && !out(ob.data(), dl)) { ok = false; break; }
                if (status == LZMA_STATUS_FINISHED_WITH_MARK) { finished = true; break; }
                if (sp >= chunk && dl < ob.size()) break;
                if (dl == 0 && sl == 0) break;
            }
            rd.advance(chunk); first = false;
            c.tick(false);
            if (ok && !c.poll()) ok = false;
        }
        Lzma2Dec_Free(&dec, &g_Alloc);
    } else {
        CLzmaDec dec; LzmaDec_CONSTRUCT(&dec);
        if (fp.props.size() < 5 || LzmaDec_Allocate(&dec, fp.props.data(), (unsigned)fp.props.size(), &g_Alloc) != SZ_OK) return c.fail("Not enough memory to decompress this archive (it needs a very large dictionary).", false);
        LzmaDec_Init(&dec);
        uint64_t raw = 0, rawTotal = fp.unpackSize;   // the filter does not change sizes
        raw = produced + fb.size();
        while (ok && raw < rawTotal) {
            rd.need(1);
            SizeT dl = (SizeT)std::min<uint64_t>(ob.size(), rawTotal - raw), sl = rd.avail(); ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            SRes r = LzmaDec_DecodeToBuf(&dec, ob.data(), &dl, rd.ptr(), &sl, dl == rawTotal - raw ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);
            rd.advance(sl);
            if (r != SZ_OK) { ok = c.fail("The archive data is damaged near " + c.pr.current + ".", false); break; }
            raw += dl;
            if (dl && !out(ob.data(), dl)) { ok = false; break; }
            if (dl == 0 && sl == 0) { ok = c.fail("The archive data ends early or cannot be read."); break; }
            c.tick(false);
            if (ok && !c.poll()) ok = false;
        }
        LzmaDec_Free(&dec, &g_Alloc);
    }
    if (!ok) return false;
    if (!fb.empty()) { if (!emit(fb.data(), fb.size())) return false; fb.clear(); }
    while (ord < fp.files.size() && items[fp.files[ord]].size == 0 && !c.cur) { if (!c.begin(items[fp.files[ord]], 0, CRC_INIT_VAL) || !c.end()) return false; ord++; }
    if (produced != fp.unpackSize || ord != fp.files.size() || c.cur) return c.fail("The archive data does not match its index.", false);
    if (c.sinceCommit >= c.interval || last) {
        XState s; s.unit = fi + 1;
        if (!c.commit(s, fp.packPos + fp.packSize)) return false;
    } else if (!c.dry) {
        // Not worth a commit yet: remember only in memory that this folder is done.
        c.st.unit = fi + 1; c.st.mid = false;
    }
    return true;
}

static bool extract7z(XCtx& c, std::vector<Item>& items, std::vector<size_t>& emptyFiles) {
    uint64_t asz = 0; c.arc.size(asz);
    SeekIn si; si.vt.Read = siRead; si.vt.Seek = siSeek; si.f = &c.arc; si.pos = 0; si.size = asz;
    CLookToRead2 look; LookToRead2_CreateVTable(&look, False);
    std::vector<uint8_t> lookBuf(1 << 18);
    look.buf = lookBuf.data(); look.bufSize = lookBuf.size(); look.realStream = &si.vt; LookToRead2_INIT(&look)
    CSzArEx db; SzArEx_Init(&db);
    SRes r = SzArEx_Open(&db, &look.vt, &g_Alloc, &g_Alloc);
    if (r != SZ_OK) {
        SzArEx_Free(&db, &g_Alloc);
        if (r == SZ_ERROR_UNSUPPORTED) return c.fail("This archive uses features NibbleZip cannot extract (for example encryption). Use 7-Zip for this one. Nothing was changed.", false);
        return c.fail("This is not a 7z archive, or its index is damaged. Nothing was changed.", false);
    }
    bool ok = true;
    std::vector<FolderPlan> plan(db.db.NumFolders);
    items.assign(db.NumFiles, Item());
    std::vector<uint16_t> nb;
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        size_t n = SzArEx_GetFileNameUtf16(&db, i, nullptr); nb.resize(n + 1); SzArEx_GetFileNameUtf16(&db, i, nb.data());
        Item& it = items[i];
        it.rel = utf8From16(nb.data(), n); it.isDir = SzArEx_IsDir(&db, i) != 0;
        it.size = db.UnpackPositions[i + 1] - db.UnpackPositions[i];
        if (SzBitWithVals_Check(&db.CRCs, i)) { it.hasCrc = true; it.crc = db.CRCs.Vals[i]; }
        if (SzBitWithVals_Check(&db.MTime, i)) { it.hasMtime = true; it.mtime = ((uint64_t)db.MTime.Vals[i].High << 32) | db.MTime.Vals[i].Low; }
        if (!it.isDir && db.FileToFolder[i] == (UInt32)-1) emptyFiles.push_back(i);
    }
    assignPaths(items, c.h.dest);
    uint64_t total = 0;
    for (UInt32 fi = 0; ok && fi < db.db.NumFolders; fi++) {
        CSzFolder fo; CSzData sd;
        sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fi];
        sd.Size = db.db.FoCodersOffsets[fi + 1] - db.db.FoCodersOffsets[fi];
        FolderPlan& fp = plan[fi];
        bool sup = SzGetNextFolderItem(&fo, &sd) == SZ_OK && fo.NumPackStreams == 1 && (fo.NumCoders == 1 || fo.NumCoders == 2);
        if (sup) {
            const CSzCoderInfo& m = fo.Coders[0];
            if (m.MethodID == 0) fp.method = M_COPY; else if (m.MethodID == 0x21) fp.method = M_LZMA2; else if (m.MethodID == 0x30101) fp.method = M_LZMA; else sup = false;
            if (sup && m.NumStreams != 1) sup = false;
            if (sup) { const Byte* pp = db.db.CodersData + db.db.FoCodersOffsets[fi] + m.PropsOffset; fp.props.assign(pp, pp + m.PropsSize); }
            if (sup && fo.NumCoders == 2) {
                const CSzCoderInfo& f2 = fo.Coders[1];
                if (f2.MethodID != 0x3030103 || f2.NumStreams != 1 || fo.PackStreams[0] != 0 || fo.NumBonds != 1 || fo.Bonds[0].InIndex != 1 || fo.Bonds[0].OutIndex != 0) sup = false;
                else fp.bcj = true;
            }
            if (sup && ((fp.method == M_LZMA2 && fp.props.size() != 1) || (fp.method == M_LZMA && fp.props.size() != 5))) sup = false;
        }
        if (!sup) { ok = c.fail("This archive uses a compression method NibbleZip cannot extract. Use 7-Zip for this one. Nothing was changed.", false); break; }
        UInt32 psi = db.db.FoStartPackStreamIndex[fi];
        fp.packPos = db.dataPos + db.db.PackPositions[psi]; fp.packSize = db.db.PackPositions[psi + 1] - db.db.PackPositions[psi];
        fp.unpackSize = SzAr_GetFolderUnpackSize(&db.db, fi);
        for (UInt32 i = db.FolderToFile[fi]; i < db.NumFiles && (fi + 1 >= db.db.NumFolders || i < db.FolderToFile[fi + 1]); i++)
            if (db.FileToFolder[i] == fi) fp.files.push_back(i);
        total += fp.unpackSize;
    }
    SzArEx_Free(&db, &g_Alloc);
    if (!ok) return false;
    c.pr.totalBytes = total * ((c.opt.precheck && c.h.eat) ? 2 : 1); c.pr.filesTotal = 0;
    for (auto& it : items) if (!it.isDir) c.pr.filesTotal++;
    c.baseDone = c.dry ? 0 : ((c.opt.precheck && c.h.eat) ? total : 0);
    for (uint64_t fi = c.st.unit; fi < plan.size(); fi++) {
        if (!decodeFolder(c, plan[fi], fi, fi + 1 == plan.size(), items)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- ZIP
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

struct ZEnt { uint16_t flags = 0, method = 0; uint64_t csize = 0, lfh = 0, dataStart = 0; size_t item = 0; };

static uint64_t dosToFiletime(uint16_t d, uint16_t t) {
    struct tm tmv; memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = ((d >> 9) & 0x7F) + 80; tmv.tm_mon = ((d >> 5) & 0xF) - 1; tmv.tm_mday = d & 0x1F;
    tmv.tm_hour = (t >> 11) & 0x1F; tmv.tm_min = (t >> 5) & 0x3F; tmv.tm_sec = (t & 0x1F) * 2; tmv.tm_isdst = -1;
    time_t tt = mktime(&tmv);
    if (tt == (time_t)-1) return 0;
    return ((uint64_t)tt + 11644473600ull) * 10000000ull;
}

static bool readZipIndex(XCtx& c, std::vector<Item>& items, std::vector<ZEnt>& ents) {
    uint64_t size = 0; c.arc.size(size);
    size_t tail = (size_t)std::min<uint64_t>(size, 65557 + 20), got = 0;
    std::vector<uint8_t> t(tail);
    if (!c.arc.readAt(size - tail, t.data(), tail, got) || got != tail) return c.fail("Could not read the archive: " + plat::lastError());
    size_t e = (size_t)-1;
    for (size_t i = tail >= 22 ? tail - 22 : 0; tail >= 22; i--) { if (rd32(&t[i]) == 0x06054b50) { e = i; break; } if (i == 0) break; }
    if (e == (size_t)-1) return c.fail("This is not a ZIP or 7z archive, or its index is missing. Nothing was changed.", false);
    uint64_t n = rd16(&t[e + 10]), cdSize = rd32(&t[e + 12]), cdOff = rd32(&t[e + 16]);
    if (e >= 20 && rd32(&t[e - 20]) == 0x07064b50) {
        uint64_t zpos = rd64(&t[e - 20 + 8]); uint8_t z[56];
        if (!c.arc.readAt(zpos, z, 56, got) || got != 56 || rd32(z) != 0x06064b50) return c.fail("The ZIP64 index is damaged. Nothing was changed.", false);
        n = rd64(z + 32); cdSize = rd64(z + 40); cdOff = rd64(z + 48);
    }
    if (cdOff + cdSize > size || cdSize > (1ull << 31)) return c.fail("The ZIP index is damaged. Nothing was changed.", false);
    std::vector<uint8_t> cd((size_t)cdSize);
    if (cdSize && (!c.arc.readAt(cdOff, cd.data(), cd.size(), got) || got != cd.size())) return c.fail("Could not read the archive: " + plat::lastError());
    size_t p = 0;
    for (uint64_t k = 0; k < n; k++) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return c.fail("The ZIP index is damaged. Nothing was changed.", false);
        ZEnt z; Item it;
        z.flags = rd16(&cd[p + 8]); z.method = rd16(&cd[p + 10]);
        uint16_t dt = rd16(&cd[p + 12]), dd = rd16(&cd[p + 14]);
        it.crc = rd32(&cd[p + 16]); it.hasCrc = true;
        uint64_t csize = rd32(&cd[p + 20]), usize = rd32(&cd[p + 24]), off = rd32(&cd[p + 42]);
        size_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
        if (p + 46 + nl + xl + cl > cd.size()) return c.fail("The ZIP index is damaged. Nothing was changed.", false);
        std::string name((const char*)&cd[p + 46], nl);
        if (!(z.flags & 0x800) && !validUtf8(name)) { std::string u; for (unsigned char ch : name) putUtf8(u, ch < 0x80 ? ch : kCp437[ch - 0x80]); name = u; }
        const uint8_t* x = &cd[p + 46 + nl]; size_t xp = 0;
        while (xp + 4 <= xl) {
            uint16_t tag = rd16(x + xp), len = rd16(x + xp + 2); const uint8_t* d = x + xp + 4; size_t dp = 0;
            if (xp + 4 + len > xl) break;
            if (tag == 1) {
                if (usize == 0xFFFFFFFFu && dp + 8 <= len) { usize = rd64(d + dp); dp += 8; }
                if (csize == 0xFFFFFFFFu && dp + 8 <= len) { csize = rd64(d + dp); dp += 8; }
                if (off == 0xFFFFFFFFu && dp + 8 <= len) { off = rd64(d + dp); dp += 8; }
            } else if (tag == 0x000A && len >= 32 && rd16(d + 4) == 1 && rd16(d + 6) >= 8) { it.mtime = rd64(d + 8); it.hasMtime = it.mtime != 0; }
            xp += 4 + len;
        }
        p += 46 + nl + xl + cl;
        if (!it.hasMtime) { it.mtime = dosToFiletime(dd, dt); it.hasMtime = it.mtime != 0; }
        it.rel = name; it.isDir = !name.empty() && (name.back() == '/' || name.back() == '\\'); it.size = usize;
        if (z.flags & 1) return c.fail("This archive is password-protected, which NibbleZip cannot extract. Use 7-Zip for this one. Nothing was changed.", false);
        if (!it.isDir && z.method != 0 && z.method != 8) return c.fail("This archive uses a compression method NibbleZip cannot extract. Use 7-Zip for this one. Nothing was changed.", false);
        z.csize = csize; z.lfh = off; z.item = items.size();
        items.push_back(it); ents.push_back(z);
    }
    std::sort(ents.begin(), ents.end(), [](const ZEnt& a, const ZEnt& b) { return a.lfh < b.lfh; });
    uint64_t prevEnd = 0;
    for (auto& z : ents) {
        uint8_t lh[30];
        // Headers of entries that were already consumed may be gone; they are only needed for what is still to come.
        if (z.lfh + 30 > size) return c.fail("The ZIP index is damaged. Nothing was changed.", false);
        bool readable = c.arc.readAt(z.lfh, lh, 30, got) && got == 30;   // not readable = in a part that was already consumed
        if (readable && rd32(lh) == 0x04034b50) z.dataStart = z.lfh + 30 + rd16(lh + 26) + rd16(lh + 28);
        else z.dataStart = 0;   // checked when the entry is reached
        if (z.lfh < prevEnd) return c.fail("This ZIP has overlapping entries and cannot be extracted safely. Nothing was changed.", false);
        prevEnd = z.dataStart ? z.dataStart + z.csize : z.lfh + 30 + z.csize;
        if (prevEnd > cdOff) return c.fail("The ZIP index is damaged. Nothing was changed.", false);
    }
    return true;
}

static bool extractZip(XCtx& c, std::vector<Item>& items) {
    std::vector<ZEnt> ents;
    if (!readZipIndex(c, items, ents)) return false;
    assignPaths(items, c.h.dest);
    uint64_t total = 0; c.pr.filesTotal = 0;
    for (auto& it : items) if (!it.isDir) { total += it.size; c.pr.filesTotal++; }
    c.pr.totalBytes = total * ((c.opt.precheck && c.h.eat) ? 2 : 1);
    c.baseDone = c.dry ? 0 : ((c.opt.precheck && c.h.eat) ? total : 0);
    std::vector<uint8_t> in(1 << 18), ob(1 << 18);
    size_t lastData = (size_t)-1;
    for (size_t k = 0; k < ents.size(); k++) if (!items[ents[k].item].isDir) lastData = k;
    for (size_t k = (size_t)c.st.unit; k < ents.size(); k++) {
        const ZEnt& z = ents[k]; const Item& it = items[z.item];
        if (it.isDir) continue;
        bool resume = c.st.mid && c.st.unit == k;
        uint64_t dataStart = resume ? c.st.fileOff : z.dataStart;   // on resume the header itself may already be released
        if (!dataStart) return c.fail("A file header inside the ZIP is damaged: " + it.rel, false);
        if (!c.begin(it, resume ? c.st.outOff : 0, resume ? c.st.crcState : CRC_INIT_VAL)) return false;
        uint64_t endIn = dataStart + z.csize, inPos = resume ? c.st.inOff : dataStart;
        if (z.method == 0) {
            if (z.csize != it.size) return c.fail("The ZIP index is inconsistent for " + it.rel, false);
            while (inPos < endIn) {
                size_t want = (size_t)std::min<uint64_t>(in.size(), endIn - inPos), got = 0;
                if (!c.arc.readAt(inPos, in.data(), want, got) || got != want) return c.fail("Could not read the archive: " + plat::lastError());
                if (!c.write(in.data(), got)) return false;
                inPos += got;
                if (!c.dry && c.sinceCommit >= c.interval && inPos < endIn) {
                    XState s; s.unit = k; s.mid = true; s.inOff = inPos; s.outOff = c.off; s.crcState = c.crc; s.fileOff = dataStart;
                    if (!c.commit(s, inPos)) return false;
                }
                c.tick(false);
                if (!c.poll()) return false;
            }
        } else {
            z_stream zs; memset(&zs, 0, sizeof(zs));
            std::vector<uint8_t> win;
            // Starts (or restarts) the decompressor at a block boundary, as a resumed run would.
            auto start = [&](uint64_t at, uint32_t bits, const std::vector<uint8_t>& window) -> bool {
                if (inflateInit2(&zs, -15) != Z_OK) return c.fail("Not enough memory.");
                if (bits) {
                    uint8_t prev = 0; size_t g = 0;
                    if (!c.arc.readAt(at - 1, &prev, 1, g) || g != 1) { inflateEnd(&zs); return c.fail("Could not read the archive: " + plat::lastError()); }
                    inflatePrime(&zs, (int)bits, prev >> (8 - bits));
                }
                if (!window.empty()) inflateSetDictionary(&zs, window.data(), (uInt)window.size());
                win = window; inPos = at; zs.avail_in = 0;
                return true;
            };
            if (!start(inPos, resume ? c.st.bits : 0, resume ? c.st.window : std::vector<uint8_t>())) return false;
            bool done = false, ok = true;
            while (ok && !done) {
                if (zs.avail_in == 0 && inPos < endIn) {
                    size_t want = (size_t)std::min<uint64_t>(in.size(), endIn - inPos), got = 0;
                    if (!c.arc.readAt(inPos, in.data(), want, got) || got != want) { ok = c.fail("Could not read the archive: " + plat::lastError()); break; }
                    zs.next_in = in.data(); zs.avail_in = (uInt)got; inPos += got;
                }
                zs.next_out = ob.data(); zs.avail_out = (uInt)ob.size();
                uInt before = zs.avail_in;
                int zr = inflate(&zs, Z_BLOCK);
                size_t o = ob.size() - zs.avail_out;
                if (o) {
                    if (!c.write(ob.data(), o)) { ok = false; break; }
                    if (o >= 32768) win.assign(ob.data() + o - 32768, ob.data() + o);
                    else { win.insert(win.end(), ob.data(), ob.data() + o); if (win.size() > 32768) win.erase(win.begin(), win.end() - 32768); }
                }
                if (zr == Z_STREAM_END) { done = true; break; }
                if (zr != Z_OK && zr != Z_BUF_ERROR) { ok = c.fail("The archive data is damaged in " + it.rel + ".", false); break; }
                if (o == 0 && before == zs.avail_in && zs.avail_in == 0 && inPos >= endIn) { ok = c.fail("The data for " + it.rel + " ends early.", false); break; }
                // End of a Deflate block that is not the last one: a safe restart point.
                if ((zs.data_type & 128) && !(zs.data_type & 64) && !c.dry && c.sinceCommit >= c.interval) {
                    XState s; s.unit = k; s.mid = true; s.inOff = inPos - zs.avail_in; s.bits = (uint32_t)(zs.data_type & 7);
                    s.outOff = c.off; s.crcState = c.crc; s.window = win; s.fileOff = dataStart;
                    uint64_t at = s.inOff; uint32_t bits = s.bits;
                    if (!c.commit(s, at - (bits ? 1 : 0))) { ok = false; break; }
                    inflateEnd(&zs);
                    if (!start(at, bits, win)) return false;
                }
                c.tick(false);
                if (!c.poll()) ok = false;
            }
            inflateEnd(&zs);
            if (!ok) return false;
        }
        if (!c.end()) return false;
        if (c.sinceCommit >= c.interval || k == lastData) {
            XState s; s.unit = k + 1;
            uint64_t upTo = endIn;
            for (size_t q = k + 1; q < ents.size(); q++) { upTo = ents[q].lfh; break; }
            if (!c.commit(s, std::max(upTo, endIn))) return false;
        } else if (!c.dry) { c.st.unit = k + 1; c.st.mid = false; }
    }
    return true;
}

static std::string stripArchiveExt(const std::string& name) {
    std::string l = lowerAscii(name);
    for (const char* e : {".7z", ".zip"}) { size_t n = strlen(e); if (l.size() > n && l.compare(l.size() - n, n, e) == 0) return name.substr(0, name.size() - n); }
    return name + " extracted";
}

} // namespace

std::string defaultExtractDest(const std::string& archive0) {
    std::string picked = plat::absPath(archive0), base; uint64_t ps = 0;
    ArcFile::detect(picked, base, ps);
    std::string dir = plat::parentDir(base), stem = stripArchiveExt(plat::baseName(base));
    std::string cand = plat::join(dir, stem); plat::Stat st;
    for (int i = 2; i < 1000 && plat::stat(cand, st); i++) cand = plat::join(dir, stem + " (" + std::to_string(i) + ")");
    return cand;
}

bool loadExtraction(const std::string& archive0, ExtractInfo& info) {
    crcInit();
    std::string picked = plat::absPath(archive0), base; uint64_t ps = 0;
    ArcFile::detect(picked, base, ps);
    XHeader h; XState st; std::vector<uint8_t> bytes;
    if (!loadX(base, h, st, bytes)) return false;
    info.archive = h.archive; info.dest = h.dest; info.eat = h.eat;
    return true;
}

std::vector<ExtractInfo> pendingExtractions() {
    crcInit();
    std::vector<ExtractInfo> out; std::vector<plat::DirEnt> ents;
    if (!plat::listDir(mirrorDir(), ents)) return out;
    for (auto& e : ents) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, XEXT) != 0) continue;
        std::vector<uint8_t> b; XHeader h; XState st;
        if (!readAll(plat::join(mirrorDir(), e.name), b) || !parseX(b, h, st)) continue;
        if (!ArcFile::exists(h.archive)) continue;
        ExtractInfo xi;
        if (loadExtraction(h.archive, xi)) out.push_back(xi);
    }
    return out;
}

ExtractResult extractArchive(const ExtractOptions& opt, Callbacks& cb) {
    crcInit(); cpInit();
    XCtx c; c.opt = opt; c.cb = &cb; c.lastTick = std::chrono::steady_clock::now();
    if (opt.testInterval) c.interval = opt.testInterval;
    auto finish = [&]() -> ExtractResult {
        if (c.f.isOpen()) c.f.close();
        c.pr.doneBytes = c.baseDone + c.st.doneOut; c.pr.filesDone = c.st.filesDone;
        c.res.final = c.pr; c.res.dest = c.h.dest; c.res.files = c.st.filesDone; c.res.bytes = c.st.doneOut;
        if (c.stop == 1 && c.res.error.empty()) { c.res.paused = true; c.res.resumable = true; }
        cb.progress(c.pr);
        return c.res;
    };
    std::string picked = plat::absPath(opt.archive), base; uint64_t partSize = 0;
    ArcFile::detect(picked, base, partSize);
    {   // the first part of a split archive that has already been removed still names its job
        plat::Stat ps;
        if (base == picked && picked.size() > 4 && picked.compare(picked.size() - 4, 4, ".001") == 0 && !plat::stat(picked, ps)) base = picked.substr(0, picked.size() - 4);
    }
    std::vector<uint8_t> jbytes; XState loaded;
    bool resume = loadX(base, c.h, loaded, jbytes);
    if (resume && !ArcFile::exists(base)) {
        // The archive was already removed at the very end of an earlier run: only the journal was left to tidy up.
        plat::removeFile(base + XEXT); plat::removeFile(mirrorPathFor(base, XEXT));
        c.st = std::move(loaded);
        c.res.ok = true; c.res.archiveRemoved = true;
        return finish();
    }
    plat::Stat st;
    if (!resume) {
        if (!ArcFile::exists(base)) { c.fail("Archive not found: " + base, false); return finish(); }
        if (plat::stat(journalPathFor(base), st)) { c.fail("This archive belongs to an unfinished compression job. Resume or close that job first.", false); return finish(); }
        c.h = XHeader(); c.h.archive = base; c.h.partSize = partSize; c.h.eat = opt.eat;
        if (!ArcFile::totalSizeOf(base, partSize, c.h.totalSize) || c.h.totalSize < 22) { c.fail("Cannot read the archive: " + base, false); return finish(); }
        c.h.dest = opt.dest.empty() ? defaultExtractDest(base) : plat::absPath(opt.dest);
    } else c.st = std::move(loaded);
    c.pr.packedBytes = c.h.totalSize;
    if (!c.arc.openForExtract(c.h.archive, c.h.partSize, c.h.totalSize, c.h.eat)) {
        c.fail(std::string("Cannot open the archive") + (c.h.eat ? " for writing (needed to release its space): " : ": ") + plat::lastError() +
               (c.h.partSize ? ". A part may be missing." : ""), resume);
        return finish();
    }
    uint8_t sig[6] = {0}; size_t got = 0;
    c.arc.readAt(0, sig, 6, got);
    static const uint8_t k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    bool is7z = got == 6 && !memcmp(sig, k7z, 6);
    std::vector<Item> items; std::vector<size_t> emptyFiles;
    auto pass = [&]() -> bool { items.clear(); emptyFiles.clear(); return is7z ? extract7z(c, items, emptyFiles) : extractZip(c, items); };

    if (!resume) {
        if (c.h.eat && opt.precheck) {
            // Prove the whole archive decompresses and matches its checksums before a single byte of it is released.
            c.dry = true;
            bool ok = pass();
            c.dry = false;
            if (!ok) { if (c.res.error.empty() && c.stop != 1) c.fail("The archive could not be checked.", false); c.res.resumable = false; c.st = XState(); return finish(); }
            c.st = XState(); c.cur = nullptr; c.sinceCommit = 0;
        }
        plat::makeDirs(c.h.dest);
        if (!plat::stat(c.h.dest, st) || !st.isDir) { c.fail("Cannot create the destination folder " + c.h.dest + ": " + plat::lastError(), false); return finish(); }
        Buf hb; hb.u8(X_HEADER); hb.u32(XMAGIC); hb.str(c.h.archive); hb.str(c.h.dest); hb.u64(c.h.partSize); hb.u64(c.h.totalSize); hb.u8(c.h.eat ? 1 : 0);
        if (!c.j.start(c.h.archive, std::vector<uint8_t>(), XEXT) || !c.j.append(hb)) { c.fail("Cannot create the journal: " + plat::lastError(), false); c.j.destroy(); return finish(); }
        CP("x-started");
    } else {
        if (!c.j.start(c.h.archive, jbytes, XEXT)) { c.fail("Cannot open the journal: " + plat::lastError()); return finish(); }
        plat::makeDirs(c.h.dest);
    }
    if (!pass()) return finish();
    if (c.stop == 1) return finish();
    // Folders and empty files have no data of their own; create them last.
    for (auto& it : items) if (it.isDir) plat::makeDirs(it.path);
    for (size_t i : emptyFiles) {
        plat::makeDirs(plat::parentDir(items[i].path));
        plat::File ef;
        if (!ef.open(items[i].path, plat::OpenRWAlways) || !ef.truncate(0)) c.warn("Could not create empty file " + items[i].path);
        ef.close();
        if (items[i].hasMtime) plat::setMTime(items[i].path, items[i].mtime);
        c.st.filesDone++;
    }
    for (size_t i = items.size(); i-- > 0;) if (items[i].isDir && items[i].hasMtime) plat::setMTime(items[i].path, items[i].mtime);
    CP("x-finalized");
    c.arc.close();
    if (c.h.eat) {
        ArcFile::removeAll(c.h.archive, c.h.partSize, c.h.totalSize);
        c.res.archiveRemoved = !ArcFile::exists(c.h.archive);
        if (!c.res.archiveRemoved) c.warn("Everything was extracted, but the archive could not be deleted: " + c.h.archive);
        c.pr.reclaimedBytes = c.h.totalSize; c.pr.packedBytes = 0;
    }
    CP("x-archive-removed");
    c.j.destroy();
    c.res.ok = true;
    return finish();
}

} // namespace nz
