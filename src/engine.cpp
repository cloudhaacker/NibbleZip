#include "engine.h"
#include "platform.h"
#include "arcfile.h"
#include "common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <thread>

#include "7zTypes.h"
#include "7zCrc.h"
#include "Alloc.h"
#include "Lzma2Dec.h"
#include "Lzma2Enc.h"
#include <zlib.h>

namespace nz {
using namespace detail;
namespace {

// ---------------------------------------------------------------- utilities
static uint64_t worstCase(uint64_t n) { return n + n / 8192 + 65536; }

struct Entry {
    std::string src, rel;
    uint64_t size = 0, mtime = 0;
    uint32_t attr = 0, crc = 0;
    uint64_t lfh = 0, csize = 0;   // ZIP only
    uint8_t method = 8, z64 = 0, isDir = 0;
};
struct Folder { uint64_t packSize = 0; uint8_t prop = 0; size_t first = 0, count = 0; };
struct PartState { bool active = false; Entry e; uint64_t offset = 0; uint32_t crcState = 0; uint64_t packStart = 0; uint8_t prop = 0; };
// One committed step of a large file, kept so the steps can be undone in reverse order.
struct PartStep { uint64_t endBefore = 0, endAfter = 0, offBefore = 0, offAfter = 0, csizeBefore = 0; uint32_t crcBefore = 0; };
struct State {
    std::vector<PartStep> partSteps;
    std::vector<Entry> entries;
    std::vector<Folder> folders;
    std::vector<Entry> empties;
    uint64_t dataEnd = 0;
    PartState part;
};
struct JHeader { int format = 0, level = 1; bool eat = true; std::string archive; std::vector<std::string> inputs; uint64_t split = 0; };

const uint32_t JMAGIC = 0x314A5A4E; // "NZJ1"
enum { R_HEADER = 1, R_BATCH = 2, R_PART = 3, R_EMPTY = 4, R_UNPART = 5 };

static void putEntry(Buf& b, const Entry& e) {
    b.str(e.src); b.str(e.rel); b.u64(e.size); b.u64(e.mtime); b.u32(e.attr); b.u32(e.crc);
    b.u64(e.lfh); b.u64(e.csize); b.u8(e.method); b.u8(e.z64); b.u8(e.isDir);
}
static Entry getEntry(Rd& r) {
    Entry e; e.src = r.str(); e.rel = r.str(); e.size = r.u64(); e.mtime = r.u64(); e.attr = r.u32(); e.crc = r.u32();
    e.lfh = r.u64(); e.csize = r.u64(); e.method = r.u8(); e.z64 = r.u8(); e.isDir = r.u8();
    return e;
}

// Replays journal bytes. Returns the length of the valid prefix (0 if no usable header).
static size_t parseJournal(const std::vector<uint8_t>& b, JHeader& h, State& st) {
    st = State(); h = JHeader();
    size_t pos = 0; bool haveHeader = false;
    while (pos + 8 <= b.size()) {
        Rd fr(b.data() + pos, 8);
        uint32_t len = fr.u32(), crc = fr.u32();
        if (len == 0 || len > (1u << 30) || pos + 8 + len > b.size()) break;
        const uint8_t* pl = b.data() + pos + 8;
        if (CrcCalc(pl, len) != crc) break;
        Rd r(pl, len);
        uint8_t type = r.u8();
        if (!haveHeader) {
            if (type != R_HEADER || r.u32() != JMAGIC) break;
            h.format = r.u8(); h.level = r.u8(); h.eat = r.u8() != 0; h.archive = r.str();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && !r.bad; i++) h.inputs.push_back(r.str());
            if (r.bad) break;
            if (r.pos + 8 <= r.n) h.split = r.u64();   // added in 0.5; older journals have no such field
            st.dataEnd = h.format == F7Z ? 32 : 0;
            haveHeader = true;
        } else if (type == R_BATCH) {
            uint64_t dataEnd = r.u64(), packSize = r.u64(); uint8_t prop = r.u8(); uint32_t n = r.u32();
            std::vector<Entry> es;
            for (uint32_t i = 0; i < n && !r.bad; i++) es.push_back(getEntry(r));
            if (r.bad) break;
            Folder f; f.packSize = packSize; f.prop = prop; f.first = st.entries.size(); f.count = es.size();
            for (auto& e : es) st.entries.push_back(std::move(e));
            st.folders.push_back(f);
            st.dataEnd = dataEnd;
        } else if (type == R_PART) {
            uint64_t dataEnd = r.u64(); Entry e = getEntry(r);
            uint64_t off = r.u64(); uint32_t crcState = r.u32(); uint64_t packStart = r.u64(); uint8_t prop = r.u8();
            if (r.bad) break;
            PartStep ps; ps.endBefore = st.dataEnd; ps.endAfter = dataEnd; ps.offAfter = off;
            if (st.part.active) { ps.offBefore = st.part.offset; ps.csizeBefore = st.part.e.csize; ps.crcBefore = st.part.crcState; }
            else { ps.offBefore = 0; ps.csizeBefore = 0; ps.crcBefore = CRC_INIT_VAL; st.partSteps.clear(); }
            st.partSteps.push_back(ps);
            st.dataEnd = dataEnd;
            if (off >= e.size) {
                st.partSteps.clear();
                e.crc = CRC_GET_DIGEST(crcState);
                Folder f; f.packSize = dataEnd - packStart; f.prop = prop; f.first = st.entries.size(); f.count = 1;
                st.entries.push_back(std::move(e)); st.folders.push_back(f);
                st.part = PartState();
            } else {
                st.part.active = true; st.part.e = std::move(e); st.part.offset = off; st.part.crcState = crcState;
                st.part.packStart = packStart; st.part.prop = prop;
            }
        } else if (type == R_UNPART) {
            // One step of the half-processed file was put back into its original.
            if (!st.part.active || st.partSteps.empty()) break;
            PartStep ps = st.partSteps.back(); st.partSteps.pop_back();
            st.dataEnd = ps.endBefore; st.part.offset = ps.offBefore; st.part.crcState = ps.crcBefore; st.part.e.csize = ps.csizeBefore;
            if (st.partSteps.empty()) st.part = PartState();
        } else if (type == R_EMPTY) {
            uint32_t n = r.u32();
            std::vector<Entry> es;
            for (uint32_t i = 0; i < n && !r.bad; i++) es.push_back(getEntry(r));
            if (r.bad) break;
            for (auto& e : es) st.empties.push_back(std::move(e));
        } else break;
        pos += 8 + len;
    }
    return haveHeader ? pos : 0;
}

// Loads whichever journal copy has the longer valid prefix.
static bool loadJournal(const std::string& archive, JHeader& h, State& st, std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> ba, bb; JHeader ha, hb; State sa, sb;
    size_t la = 0, lb = 0;
    if (readAll(journalPathFor(archive), ba)) la = parseJournal(ba, ha, sa);
    if (readAll(mirrorPathFor(archive), bb)) {
        lb = parseJournal(bb, hb, sb);
        if (lb && !plat::samePath(hb.archive, archive)) lb = 0;
    }
    if (!la && !lb) return false;
    if (la >= lb) { h = ha; st = std::move(sa); ba.resize(la); bytes = std::move(ba); }
    else { h = hb; st = std::move(sb); bb.resize(lb); bytes = std::move(bb); }
    return true;
}

// ---------------------------------------------------------------- scanning
struct PlanFile { std::string src, rel; uint64_t size = 0, mtime = 0; uint32_t attr = 0; };
struct Plan {
    std::vector<PlanFile> files;
    std::vector<PlanFile> dirs;
    std::vector<std::string> skipped;
    uint64_t total = 0, largest = 0;
    std::vector<std::string> jobFiles;   // journals of other unfinished jobs found among the inputs
};

static std::string g_scanArchiveKey;   // parts of the archive being written are never treated as input
static bool isJobFile(const std::string& name) {
    if (name.size() < 4) return false;
    std::string e = name.substr(name.size() - 4);
    for (auto& ch : e) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    return e == ".nzj" || e == ".nzx";
}
static bool excluded(const std::set<std::string>& exclude, const std::string& p) {
    std::string k = plat::pathKey(p);
    return exclude.count(k) || (!g_scanArchiveKey.empty() && ArcFile::isPartOf(k, g_scanArchiveKey));
}
static void scanDir(const std::string& dir, const std::string& rel, const std::set<std::string>& exclude, Plan& plan) {
    std::vector<plat::DirEnt> ents;
    if (!plat::listDir(dir, ents)) { plan.skipped.push_back("Could not read folder: " + dir); return; }
    for (auto& e : ents) {
        std::string p = plat::join(dir, e.name), r = rel + "/" + e.name;
        if (e.st.isLink) { plan.skipped.push_back("Skipped link/cloud placeholder: " + p); continue; }
        if (e.st.isDir) {
            PlanFile d; d.src = p; d.rel = r; d.mtime = e.st.mtime; d.attr = e.st.attr;
            plan.dirs.push_back(d);
            scanDir(p, r, exclude, plan);
        } else {
            if (excluded(exclude, p)) continue;
            if (isJobFile(e.name)) { plan.jobFiles.push_back(p); continue; }
            PlanFile f; f.src = p; f.rel = r; f.size = e.st.size; f.mtime = e.st.mtime; f.attr = e.st.attr;
            plan.total += f.size; plan.largest = std::max(plan.largest, f.size);
            plan.files.push_back(std::move(f));
        }
    }
}

static bool scan(const std::vector<std::string>& inputs, const std::string& archive, Plan& plan, std::string& err, bool allowMissing) {
    std::set<std::string> exclude;
    exclude.insert(plat::pathKey(archive));
    exclude.insert(plat::pathKey(journalPathFor(archive)));
    g_scanArchiveKey = plat::pathKey(archive);
    std::set<std::string> names;
    for (auto& in0 : inputs) {
        std::string in = plat::absPath(in0);
        plat::Stat st;
        if (!plat::stat(in, st)) {
            if (allowMissing) continue;   // already eaten in an earlier session
            err = "Not found: " + in; return false;
        }
        std::string name = plat::baseName(in);
        if (name.empty()) { err = "Cannot archive a whole drive root: " + in; return false; }
        if (!names.insert(plat::pathKey("/" + name)).second) { err = "Two selected items share the name \"" + name + "\""; return false; }
        if (st.isLink) { plan.skipped.push_back("Skipped link/cloud placeholder: " + in); continue; }
        if (st.isDir) {
            PlanFile d; d.src = in; d.rel = name; d.mtime = st.mtime; d.attr = st.attr;
            plan.dirs.push_back(d);
            scanDir(in, name, exclude, plan);
        } else {
            if (excluded(exclude, in)) continue;
            if (isJobFile(name)) { plan.jobFiles.push_back(in); continue; }
            PlanFile f; f.src = in; f.rel = name; f.size = st.size; f.mtime = st.mtime; f.attr = st.attr;
            plan.total += f.size; plan.largest = std::max(plan.largest, f.size);
            plan.files.push_back(std::move(f));
        }
    }
    return true;
}

// ---------------------------------------------------------------- step sizing
struct StepCfg { uint32_t dict = 0; uint64_t block = 0; int blockThreads = 1, totalThreads = 1; uint64_t step = 0; int lzLevel = 5, zLevel = 6; };

static StepCfg makeCfg(const Options& o, uint64_t freeBytes) {
    static const int lzLevels[4] = {3, 5, 7, 9};
    static const uint32_t dicts[4] = {4u << 20, 16u << 20, 32u << 20, 64u << 20};
    static const uint64_t blocks[4] = {16ull << 20, 64ull << 20, 128ull << 20, 192ull << 20};
    static const int zLevels[4] = {3, 6, 9, 9};
    int lv = std::min(3, std::max(0, o.level));
    StepCfg c; unsigned cpus = plat::cpuCount();
    if (const char* e = getenv("NIBBLEZIP_CPUS")) { int v = atoi(e); if (v > 0) cpus = (unsigned)v; }
    c.lzLevel = lzLevels[lv]; c.zLevel = zLevels[lv]; c.dict = dicts[lv];
    if (o.format == F7Z) {
        c.block = blocks[lv];
        uint64_t budget = plat::physMemory() / 2;
        uint64_t per = 3 * c.block + 12ull * c.dict;
        int t = (int)std::max<uint64_t>(1, budget / per);
        t = std::min(t, (int)std::max(1u, cpus / 2)); t = std::min(t, 8);
        while (t > 1 && (uint64_t)t * c.block > (1ull << 30)) t--;
        c.blockThreads = t; c.totalThreads = (int)std::min<unsigned>(cpus, (unsigned)t * 2);
    } else {
        c.block = 8ull << 20;
        c.totalThreads = (int)std::min(cpus, 16u);
        c.blockThreads = std::max(1, std::min(c.totalThreads * 2, 16));
    }
    if (o.testStep) { c.block = o.testStep; c.blockThreads = 1; if (o.format == F7Z) c.dict = (uint32_t)std::min<uint64_t>(c.dict, std::max<uint64_t>(o.testStep, 4096)); }
    // Shrink the step until one worst-case step fits in the free space.
    const uint64_t margin = 16ull << 20;
    while (!o.testStep && worstCase((uint64_t)c.blockThreads * c.block) + margin > freeBytes) {
        if (c.blockThreads > 1) c.blockThreads--;
        else if (c.block > (4ull << 20)) c.block /= 2;
        else break;
    }
    c.step = (uint64_t)c.blockThreads * c.block;
    return c;
}

// ---------------------------------------------------------------- codecs
struct Ctx;
static void tick(Ctx& c, bool force);

struct OutVec { ISeqOutStream vt; std::vector<uint8_t>* v; };
static size_t outWrite(ISeqOutStreamPtr pp, const void* data, size_t size) {
    OutVec* o = Z7_CONTAINER_FROM_VTBL(pp, OutVec, vt);
    try { o->v->insert(o->v->end(), (const uint8_t*)data, (const uint8_t*)data + size); } catch (...) { return 0; }
    return size;
}

struct Ctx {
    Options opt; Callbacks* cb = nullptr; State st; ArcFile arc; Journal j; Progress pr; Result res;
    StepCfg cfg; std::string archive, arcDir; int stop = 0; bool holes = true;
    uint64_t stepBase = 0, stepOut = 0; uint64_t idxEst = 4096; bool journalBroken = false;
    std::chrono::steady_clock::time_point lastTick;
    std::vector<uint8_t> buf, out;
    bool fail(const std::string& msg, bool resumable = true) { if (res.error.empty()) { res.error = msg; res.resumable = resumable; } return false; }
    void warn(const std::string& w) { if (res.warnings.size() < 500) res.warnings.push_back(w); }
};
static void tick(Ctx& c, bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - c.lastTick < std::chrono::milliseconds(120)) return;
    c.lastTick = now;
    c.pr.packedBytes = c.st.dataEnd + c.stepOut;
    c.cb->progress(c.pr);
}
static void pollStop(Ctx& c) {
    int m = c.cb->poll();
    if (m == 1) c.stop = 1; else if (m == 2 && c.stop == 0) c.stop = 2;
}

struct Prog { ICompressProgress vt; Ctx* c; };
static SRes progFn(ICompressProgressPtr pp, UInt64 inSize, UInt64 outSize) {
    Prog* p = Z7_CONTAINER_FROM_VTBL(pp, Prog, vt);
    Ctx& c = *p->c;
    if (inSize != (UInt64)(Int64)-1) c.pr.doneBytes = c.stepBase + inSize;
    if (outSize != (UInt64)(Int64)-1) c.stepOut = outSize;
    tick(c, false);
    pollStop(c);
    return c.stop == 1 ? SZ_ERROR_PROGRESS : SZ_OK;
}

// Encodes one independent LZMA2 run. Non-final runs have the end marker removed so that
// consecutive runs concatenate into one valid LZMA2 stream.
static bool lzmaEncode(Ctx& c, const uint8_t* in, size_t n, bool final, bool reduce, std::vector<uint8_t>& out, uint8_t& prop) {
    CLzma2EncHandle h = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
    if (!h) return c.fail("Out of memory creating the compressor");
    CLzma2EncProps p; Lzma2EncProps_Init(&p);
    p.lzmaProps.level = c.cfg.lzLevel; p.lzmaProps.dictSize = c.cfg.dict;
    if (reduce) p.lzmaProps.reduceSize = n;
    p.blockSize = c.cfg.block; p.numBlockThreads_Max = c.cfg.blockThreads; p.numTotalThreads = c.cfg.totalThreads;
    SRes r = Lzma2Enc_SetProps(h, &p);
    if (r != SZ_OK) { Lzma2Enc_Destroy(h); return c.fail("Compressor rejected its settings"); }
    prop = Lzma2Enc_WriteProperties(h);
    out.clear();
    try { out.reserve(n / 2 + 65536); } catch (...) {}
    OutVec ov; ov.vt.Write = outWrite; ov.v = &out;
    Prog pg; pg.vt.Progress = progFn; pg.c = &c;
    r = Lzma2Enc_Encode2(h, &ov.vt, nullptr, nullptr, nullptr, in, n, &pg.vt);
    Lzma2Enc_Destroy(h);
    if (r == SZ_ERROR_PROGRESS || c.stop == 1) { c.stop = 1; return false; }
    if (r == SZ_ERROR_MEM) return c.fail("Not enough memory for this compression level. Try a lower level.");
    if (r != SZ_OK) return c.fail("Compression failed (code " + std::to_string(r) + ")");
    if (out.empty() || out.back() != 0) return c.fail("Internal error: compressed stream has no end marker");
    if (!final) out.pop_back();
    return true;
}

// Decompresses what was read back from disk and compares it byte-for-byte with the source.
static bool lzmaVerify(const uint8_t* packed, size_t pn, uint8_t prop, const uint8_t* expect, size_t n, bool final) {
    CLzma2Dec d; Lzma2Dec_CONSTRUCT(&d);
    if (Lzma2Dec_Allocate(&d, prop, &g_Alloc) != SZ_OK) return false;
    Lzma2Dec_Init(&d);
    std::vector<uint8_t> tmp(1 << 20);
    size_t sp = 0, dp = 0; bool ok = false;
    for (;;) {
        SizeT dl = tmp.size(), sl = pn - sp; ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        SRes r = Lzma2Dec_DecodeToBuf(&d, tmp.data(), &dl, packed + sp, &sl, LZMA_FINISH_ANY, &status);
        if (r != SZ_OK) break;
        if (dl) { if (dp + dl > n || memcmp(tmp.data(), expect + dp, dl) != 0) break; dp += dl; }
        sp += sl;
        if (status == LZMA_STATUS_FINISHED_WITH_MARK) { ok = final && dp == n && sp == pn; break; }
        if (dl == 0 && sl == 0) { ok = !final && dp == n && sp == pn; break; }
    }
    Lzma2Dec_Free(&d, &g_Alloc);
    return ok;
}

// Smallest LZMA2 dictionary property byte that covers `dict` bytes.
static uint8_t lzma2PropFor(uint64_t dict) {
    for (unsigned p = 0; p < 40; p++) if ((((uint64_t)2 | (p & 1)) << (p / 2 + 11)) >= dict) return (uint8_t)p;
    return 40;
}
// Quick test on evenly spaced samples: how well does this data compress? (1.0 = not at all)
static double sampleRatio(const uint8_t* data, size_t n) {
    const size_t piece = 32768;
    if (n < 2 * piece) return 0.0;
    size_t k = std::min<size_t>(24, n / piece), in = 0, out = 0;
    std::vector<uint8_t> tmp(piece + 1024);
    for (size_t i = 0; i < k; i++) {
        size_t at = (size_t)((uint64_t)(n - piece) * i / (k > 1 ? k - 1 : 1));
        z_stream z; memset(&z, 0, sizeof(z));
        if (deflateInit2(&z, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 0.0;
        z.next_in = (Bytef*)(data + at); z.avail_in = (uInt)piece; z.next_out = tmp.data(); z.avail_out = (uInt)tmp.size();
        deflate(&z, Z_FINISH);
        in += piece; out += tmp.size() - z.avail_out;
        deflateEnd(&z);
    }
    return in ? (double)out / (double)in : 0.0;
}
const double kStoreRatio = 0.97;   // above this, compressing is a waste of time: store the data as it is
// Writes data as uncompressed LZMA2 chunks (a valid LZMA2 stream any 7z reader understands).
static bool storeLzma2(const uint8_t* in, size_t n, bool final, std::vector<uint8_t>& out) {
    out.clear();
    try { out.reserve(n + n / 65536 * 3 + 16); } catch (...) { return false; }
    size_t pos = 0; bool first = true;
    while (pos < n) {
        size_t c = std::min<size_t>(65536, n - pos);
        out.push_back(first ? 0x01 : 0x02); out.push_back((uint8_t)((c - 1) >> 8)); out.push_back((uint8_t)(c - 1));
        out.insert(out.end(), in + pos, in + pos + c);
        pos += c; first = false;
    }
    if (final) out.push_back(0);
    return true;
}

// Decompresses one LZMA2 run (as written by one step) into dst; must produce exactly n bytes.
static bool lzmaDecodeRun(const uint8_t* packed, size_t pn, uint8_t prop, uint8_t* dst, size_t n) {
    CLzma2Dec d; Lzma2Dec_CONSTRUCT(&d);
    if (Lzma2Dec_Allocate(&d, prop, &g_Alloc) != SZ_OK) return false;
    Lzma2Dec_Init(&d);
    size_t sp = 0, dp = 0; bool ok = false;
    for (;;) {
        SizeT dl = n - dp, sl = pn - sp; ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        SRes r = Lzma2Dec_DecodeToBuf(&d, dst + dp, &dl, packed + sp, &sl, LZMA_FINISH_ANY, &status);
        if (r != SZ_OK) break;
        dp += dl; sp += sl;
        if (dp == n && sp == pn) { ok = true; break; }
        if (dl == 0 && sl == 0) break;
    }
    Lzma2Dec_Free(&d, &g_Alloc);
    return ok;
}
static bool inflateRun(const uint8_t* packed, size_t pn, uint8_t* dst, size_t n) {
    z_stream s; memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) return false;
    s.next_in = (Bytef*)packed; s.avail_in = (uInt)pn; s.next_out = dst; s.avail_out = (uInt)n;
    int r = inflate(&s, Z_SYNC_FLUSH);
    bool ok = (r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR) && s.avail_out == 0;
    if (ok && s.avail_in > 0) {   // the step ends with an empty flush block that produces no data
        uint8_t scratch[16]; s.next_out = scratch; s.avail_out = sizeof(scratch);
        r = inflate(&s, Z_SYNC_FLUSH);
        ok = (r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR) && s.avail_out == sizeof(scratch) && s.avail_in == 0;
    }
    inflateEnd(&s);
    return ok;
}

struct ZJob { const uint8_t* in = nullptr; size_t n = 0; int flush = Z_FINISH; std::vector<uint8_t> out; bool ok = false; };
static void deflateOne(ZJob& j, int level) {
    z_stream s; memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return;
    try { j.out.resize(deflateBound(&s, (uLong)j.n) + 64); } catch (...) { deflateEnd(&s); return; }
    s.next_in = (Bytef*)j.in; s.avail_in = (uInt)j.n; s.next_out = j.out.data(); s.avail_out = (uInt)j.out.size();
    int r = deflate(&s, j.flush);
    bool ok = (j.flush == Z_FINISH) ? (r == Z_STREAM_END) : (r == Z_OK && s.avail_in == 0 && s.avail_out != 0);
    j.out.resize(j.out.size() - s.avail_out);
    deflateEnd(&s);
    j.ok = ok;
}
static void runZJobs(std::vector<ZJob>& jobs, int level, int threads) {
    std::atomic<size_t> next(0);
    auto work = [&]() { for (;;) { size_t i = next.fetch_add(1); if (i >= jobs.size()) break; deflateOne(jobs[i], level); } };
    int t = (int)std::min<size_t>((size_t)std::max(1, threads), jobs.size());
    std::vector<std::thread> th;
    for (int i = 1; i < t; i++) th.emplace_back(work);
    work();
    for (auto& x : th) x.join();
}
static bool inflateVerify(const uint8_t* packed, size_t pn, const uint8_t* expect, size_t n, bool final) {
    z_stream s; memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) return false;
    std::vector<uint8_t> tmp(1 << 20);
    size_t dp = 0; bool ok = false;
    s.next_in = (Bytef*)packed; s.avail_in = (uInt)pn;
    for (;;) {
        s.next_out = tmp.data(); s.avail_out = (uInt)tmp.size();
        int r = inflate(&s, Z_NO_FLUSH);
        size_t got = tmp.size() - s.avail_out;
        if (got) { if (dp + got > n || memcmp(tmp.data(), expect + dp, got) != 0) break; dp += got; }
        if (r == Z_STREAM_END) { ok = final && dp == n && s.avail_in == 0; break; }
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (got == 0 && s.avail_in == 0) { ok = !final && dp == n; break; }
        if (got == 0 && r == Z_BUF_ERROR) break;
    }
    inflateEnd(&s);
    return ok;
}

// ---------------------------------------------------------------- 7z index
static void w7num(Buf& b, uint64_t v) {
    uint8_t first = 0, mask = 0x80; int i;
    for (i = 0; i < 8; i++) {
        if (v < ((uint64_t)1 << (7 * (i + 1)))) { first |= (uint8_t)(v >> (8 * i)); break; }
        first |= mask; mask >>= 1;
    }
    b.u8(first);
    for (; i > 0; i--) { b.u8((uint8_t)v); v >>= 8; }
}
static void utf16(Buf& b, const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t c = (unsigned char)s[i]; int extra = 0;
        if (c < 0x80) extra = 0; else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; } else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else { c = 0xFFFD; extra = 0; }
        i++;
        for (int k = 0; k < extra; k++) { if (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) c = (c << 6) | ((unsigned char)s[i++] & 0x3F); else { c = 0xFFFD; break; } }
        if (c >= 0x10000) { c -= 0x10000; b.u16((uint16_t)(0xD800 + (c >> 10))); b.u16((uint16_t)(0xDC00 + (c & 0x3FF))); }
        else b.u16((uint16_t)c);
    }
    b.u16(0);
}
static void bitvec(Buf& b, const std::vector<bool>& v) {
    uint8_t cur = 0, mask = 0x80;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i]) cur |= mask;
        mask >>= 1;
        if (!mask) { b.u8(cur); cur = 0; mask = 0x80; }
    }
    if (mask != 0x80) b.u8(cur);
}

static std::vector<uint8_t> build7zHeader(const State& st, const std::vector<Entry>& empties) {
    Buf b;
    size_t nStream = st.entries.size(), nAll = nStream + empties.size();
    if (nAll == 0) return b.b;
    b.u8(0x01);
    if (!st.folders.empty()) {
        b.u8(0x04);
        b.u8(0x06); w7num(b, 0); w7num(b, st.folders.size());
        b.u8(0x09); for (auto& f : st.folders) w7num(b, f.packSize);
        b.u8(0x00);
        b.u8(0x07); b.u8(0x0B); w7num(b, st.folders.size()); b.u8(0);
        for (auto& f : st.folders) { w7num(b, 1); b.u8(0x21); b.u8(0x21); w7num(b, 1); b.u8(f.prop); }
        b.u8(0x0C);
        for (auto& f : st.folders) { uint64_t sum = 0; for (size_t i = 0; i < f.count; i++) sum += st.entries[f.first + i].size; w7num(b, sum); }
        b.u8(0x00);
        b.u8(0x08);
        b.u8(0x0D); for (auto& f : st.folders) w7num(b, f.count);
        b.u8(0x09); for (auto& f : st.folders) for (size_t i = 0; i + 1 < f.count; i++) w7num(b, st.entries[f.first + i].size);
        b.u8(0x0A); b.u8(1); for (auto& e : st.entries) b.u32(e.crc);
        b.u8(0x00);
        b.u8(0x00);
    }
    b.u8(0x05); w7num(b, nAll);
    if (!empties.empty()) {
        std::vector<bool> es(nAll, false), ef;
        for (size_t i = nStream; i < nAll; i++) es[i] = true;
        for (auto& e : empties) ef.push_back(!e.isDir);
        b.u8(0x0E); w7num(b, (nAll + 7) / 8); bitvec(b, es);
        b.u8(0x0F); w7num(b, (ef.size() + 7) / 8); bitvec(b, ef);
    }
    Buf names;
    for (auto& e : st.entries) utf16(names, e.rel);
    for (auto& e : empties) utf16(names, e.rel);
    b.u8(0x11); w7num(b, names.b.size() + 1); b.u8(0); b.bytes(names.b.data(), names.b.size());
    b.u8(0x14); w7num(b, 2 + 8 * nAll); b.u8(1); b.u8(0);
    for (auto& e : st.entries) b.u64(e.mtime);
    for (auto& e : empties) b.u64(e.mtime);
    b.u8(0x15); w7num(b, 2 + 4 * nAll); b.u8(1); b.u8(0);
    for (auto& e : st.entries) b.u32(e.attr);
    for (auto& e : empties) b.u32(e.attr);
    b.u8(0x00);
    b.u8(0x00);
    return b.b;
}
static std::vector<uint8_t> build7zStart(uint64_t nextOff, uint64_t nextSize, uint32_t nextCrc) {
    Buf t; t.u64(nextOff); t.u64(nextSize); t.u32(nextCrc);
    Buf b; static const uint8_t sig[8] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4};
    b.bytes(sig, 8); b.u32(CrcCalc(t.b.data(), t.b.size())); b.bytes(t.b.data(), t.b.size());
    return b.b;
}

// ---------------------------------------------------------------- ZIP index
static void dosTime(uint64_t ft, uint16_t& d, uint16_t& t) {
    d = (1 << 5) | 1; t = 0;
    if (ft < 116444736000000000ull) return;
    time_t tt = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &tt) != 0) return;
#else
    if (!localtime_r(&tt, &lt)) return;
#endif
    if (lt.tm_year < 80) return;
    d = (uint16_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    t = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}
static std::string zipName(const Entry& e) { return e.isDir ? e.rel + "/" : e.rel; }
static void zipLFH(Buf& b, const Entry& e) {
    std::string name = zipName(e); uint16_t d, t; dosTime(e.mtime, d, t);
    b.u32(0x04034b50); b.u16(e.z64 ? 45 : 20); b.u16(0x0800); b.u16(e.method); b.u16(t); b.u16(d);
    b.u32(e.crc); b.u32(e.z64 ? 0xFFFFFFFFu : (uint32_t)e.csize); b.u32(e.z64 ? 0xFFFFFFFFu : (uint32_t)e.size);
    b.u16((uint16_t)name.size()); b.u16(e.z64 ? 20 : 0); b.bytes(name.data(), name.size());
    if (e.z64) { b.u16(0x0001); b.u16(16); b.u64(e.size); b.u64(e.csize); }
}

static std::vector<uint8_t> buildZipIndex(const std::vector<const Entry*>& all, uint64_t cdOffset) {
    Buf b;
    for (const Entry* pe : all) {
        const Entry& e = *pe;
        std::string name = zipName(e); uint16_t d, t; dosTime(e.mtime, d, t);
        bool offBig = e.lfh >= 0xFFFFFFFFull;
        Buf x;
        if (e.z64 || offBig) {
            Buf z; if (e.z64) { z.u64(e.size); z.u64(e.csize); } if (offBig) z.u64(e.lfh);
            x.u16(0x0001); x.u16((uint16_t)z.b.size()); x.bytes(z.b.data(), z.b.size());
        }
        x.u16(0x000A); x.u16(32); x.u32(0); x.u16(1); x.u16(24); x.u64(e.mtime); x.u64(e.mtime); x.u64(e.mtime);
        b.u32(0x02014b50); b.u16(40); b.u16((e.z64 || offBig) ? 45 : 20); b.u16(0x0800); b.u16(e.method); b.u16(t); b.u16(d);
        b.u32(e.crc); b.u32(e.z64 ? 0xFFFFFFFFu : (uint32_t)e.csize); b.u32(e.z64 ? 0xFFFFFFFFu : (uint32_t)e.size);
        b.u16((uint16_t)name.size()); b.u16((uint16_t)x.b.size()); b.u16(0); b.u16(0); b.u16(0);
        b.u32((e.attr & 0xFFFF) | (e.isDir ? 0x10 : 0)); b.u32(offBig ? 0xFFFFFFFFu : (uint32_t)e.lfh);
        b.bytes(name.data(), name.size()); b.bytes(x.b.data(), x.b.size());
    }
    uint64_t cdSize = b.b.size(), n = all.size();
    bool z64 = n >= 0xFFFF || cdSize >= 0xFFFFFFFFull || cdOffset >= 0xFFFFFFFFull;
    if (z64) {
        uint64_t z64pos = cdOffset + cdSize;
        b.u32(0x06064b50); b.u64(44); b.u16(45); b.u16(45); b.u32(0); b.u32(0); b.u64(n); b.u64(n); b.u64(cdSize); b.u64(cdOffset);
        b.u32(0x07064b50); b.u32(0); b.u64(z64pos); b.u32(1);
    }
    b.u32(0x06054b50); b.u16(0); b.u16(0);
    b.u16(z64 ? 0xFFFF : (uint16_t)n); b.u16(z64 ? 0xFFFF : (uint16_t)n);
    b.u32(z64 ? 0xFFFFFFFFu : (uint32_t)cdSize); b.u32(z64 ? 0xFFFFFFFFu : (uint32_t)cdOffset); b.u16(0);
    return b.b;
}

// Writes the archive index after the data so the file is a valid archive of everything committed so far.
static bool writeIndex(Ctx& c, bool final) {
    static const std::vector<Entry> none;
    const std::vector<Entry>& empties = final ? c.st.empties : none;
    if (c.opt.format == F7Z) {
        std::vector<uint8_t> hdr = build7zHeader(c.st, empties);
        if (!hdr.empty() && !c.arc.writeAt(c.st.dataEnd, hdr.data(), hdr.size())) return c.fail("Could not write archive index: " + plat::lastError());
        if (!c.arc.truncate(c.st.dataEnd + hdr.size())) return c.fail("Could not size archive: " + plat::lastError());
        if (!c.arc.flush()) return c.fail("Could not flush archive: " + plat::lastError());
        CP("index-body");
        std::vector<uint8_t> sh = build7zStart(c.st.dataEnd - 32, hdr.size(), hdr.empty() ? 0 : CrcCalc(hdr.data(), hdr.size()));
        if (!c.arc.writeAt(0, sh.data(), sh.size()) || !c.arc.flush()) return c.fail("Could not write archive header: " + plat::lastError());
        c.idxEst = hdr.size() + 4096;
    } else {
        Buf blk; std::vector<Entry> emp = empties; uint64_t pos = c.st.dataEnd;
        for (auto& e : emp) { e.lfh = pos; e.method = 0; e.csize = 0; e.crc = 0; e.z64 = 0; size_t before = blk.b.size(); zipLFH(blk, e); pos += blk.b.size() - before; }
        std::vector<const Entry*> all;
        for (auto& e : c.st.entries) all.push_back(&e);
        for (auto& e : emp) all.push_back(&e);
        std::vector<uint8_t> cd = buildZipIndex(all, pos);
        blk.bytes(cd.data(), cd.size());
        if (!c.arc.writeAt(c.st.dataEnd, blk.b.data(), blk.b.size())) return c.fail("Could not write archive index: " + plat::lastError());
        if (!c.arc.truncate(c.st.dataEnd + blk.b.size())) return c.fail("Could not size archive: " + plat::lastError());
        if (!c.arc.flush()) return c.fail("Could not flush archive: " + plat::lastError());
        c.idxEst = blk.b.size() + 4096;
    }
    return true;
}

// ---------------------------------------------------------------- committing data
// Writes c.out at the end of the archive data, forces it to disk and reads it back.
// On return c.out holds what the disk returned.
static bool commitData(Ctx& c) {
    uint64_t fr = 0;
    if (plat::freeSpace(c.arcDir, fr) && fr < c.out.size() + c.idxEst + (4ull << 20))
        return c.fail("Not enough free space on the archive drive to continue (" + humanSize(fr) + " free). Free some space and resume.");
    uint32_t crc = CrcCalc(c.out.data(), c.out.size());
    if (!c.arc.writeAt(c.st.dataEnd, c.out.data(), c.out.size())) return c.fail("Write to archive failed: " + plat::lastError());
    CP("data-written");
    if (!c.arc.flush()) return c.fail("Flushing archive to disk failed: " + plat::lastError());
    size_t got = 0;
    if (!c.arc.readAt(c.st.dataEnd, c.out.data(), c.out.size(), got) || got != c.out.size()) return c.fail("Reading archive back failed: " + plat::lastError());
    if (CrcCalc(c.out.data(), c.out.size()) != crc) return c.fail("Verification failed: archive data read back from disk does not match what was written. Nothing was deleted.");
    return true;
}

// After a failed step the old index may have been overwritten; put it back when possible.
static void restoreIndex(Ctx& c) {
    if (c.st.part.active || c.journalBroken || !c.arc.isOpen()) return;
    std::string keepErr = c.res.error; bool keepRes = c.res.resumable;
    writeIndex(c, false);
    c.res.error = keepErr; c.res.resumable = keepRes;
}

// Appends a line to the deletion log for every original that is removed.
static void logDeleted(Ctx& c, const std::string& path, uint64_t size) {
    static bool dirMade = false;
    if (!dirMade) { dirMade = true; plat::makeDirs(plat::appDataDir()); }
    plat::File lf; uint64_t pos = 0;
    if (!lf.open(deletionLogPath(), plat::OpenRWAlways) || !lf.size(pos)) return;
    time_t now = time(nullptr); struct tm lt; char ts[32] = "";
#ifdef _WIN32
    if (localtime_s(&lt, &now) == 0)
#else
    if (localtime_r(&now, &lt))
#endif
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
    std::string line = std::string(ts) + "   deleted   (" + humanSize(size) + ")\r\n    " + path + "\r\n    stored in: " + c.archive +
                       (c.opt.split ? ".001 (and following parts)" : "") + "\r\n\r\n";
    lf.writeAt(pos, line.data(), line.size());
}

static void deleteSource(Ctx& c, const Entry& e) {
    plat::Stat s;
    if (!plat::stat(e.src, s)) return;
    if (s.size != e.size || (!e.z64 && s.mtime != e.mtime)) { c.warn("Changed after it was archived, so it was kept: " + e.src); return; }
    if (plat::removeFile(e.src)) { c.pr.reclaimedBytes += e.size; logDeleted(c, e.src, e.size); }
    else c.warn("Archived but could not be deleted (" + plat::lastError() + "): " + e.src);
}

// Small files: gather up to one step of data, compress it together, verify, then delete the sources.
static bool processBatch(Ctx& c, const std::vector<PlanFile>& files) {
    c.buf.clear();
    std::vector<Entry> ents; std::vector<size_t> offs;
    for (auto& f : files) {
        plat::Stat s; plat::File in;
        auto skip = [&](const std::string& why) { c.warn(why + ": " + f.src); c.pr.totalBytes -= std::min(c.pr.totalBytes, f.size); if (c.pr.filesTotal) c.pr.filesTotal--; };
        if (!plat::stat(f.src, s) || s.isDir || s.isLink) { skip("No longer available, skipped"); continue; }
        if (s.size > f.size && s.size > c.cfg.step) { skip("Grew while the job was running, skipped"); continue; }
        if (!in.open(f.src, plat::OpenRead)) { skip("In use or unreadable, skipped (" + plat::lastError() + ")"); continue; }
        size_t at = c.buf.size(), got = 0;
        try { c.buf.resize(at + (size_t)s.size); } catch (...) { return c.fail("Out of memory reading files"); }
        if ((s.size && !in.readAt(0, c.buf.data() + at, (size_t)s.size, got)) || got != s.size) {
            c.buf.resize(at); skip("Read error, skipped (" + plat::lastError() + ")"); continue;
        }
        Entry e; e.src = f.src; e.rel = f.rel; e.size = s.size; e.mtime = s.mtime; e.attr = s.attr;
        e.crc = CrcCalc(c.buf.data() + at, (size_t)s.size);
        if (s.size == 0) { bool dup = false; for (auto& x : c.st.empties) if (x.rel == e.rel) dup = true; if (!dup) c.st.empties.push_back(e); continue; }
        ents.push_back(std::move(e)); offs.push_back(at);
    }
    if (ents.empty()) return true;
    c.pr.current = ents.size() == 1 ? ents[0].rel : ents[0].rel + "  (+" + std::to_string(ents.size() - 1) + " more)";
    c.stepBase = c.pr.doneBytes; c.stepOut = 0;
    size_t n = c.buf.size();
    Folder fo; fo.first = c.st.entries.size(); fo.count = ents.size();
    if (c.opt.format == F7Z) {
        if (sampleRatio(c.buf.data(), n) > kStoreRatio) {
            fo.prop = lzma2PropFor(std::min<uint64_t>(c.cfg.dict, std::max<uint64_t>(n, 4096)));
            if (!storeLzma2(c.buf.data(), n, true, c.out)) return c.fail("Out of memory");
        } else if (!lzmaEncode(c, c.buf.data(), n, true, true, c.out, fo.prop)) return false;
        fo.packSize = c.out.size();
        if (!commitData(c)) return false;
        if (!lzmaVerify(c.out.data(), c.out.size(), fo.prop, c.buf.data(), n, true))
            return c.fail("Verification failed: decompressed data does not match the source files. Nothing was deleted.");
    } else {
        std::vector<ZJob> jobs(ents.size());
        std::vector<ZJob> work; std::vector<size_t> workIdx; std::vector<char> forceStore(ents.size(), 0);
        for (size_t i = 0; i < ents.size(); i++) {
            jobs[i].in = c.buf.data() + offs[i]; jobs[i].n = (size_t)ents[i].size; jobs[i].flush = Z_FINISH;
            if (sampleRatio(jobs[i].in, jobs[i].n) > kStoreRatio) { jobs[i].ok = true; forceStore[i] = 1; }
            else { work.push_back(ZJob()); work.back().in = jobs[i].in; work.back().n = jobs[i].n; workIdx.push_back(i); }
        }
        runZJobs(work, c.cfg.zLevel, c.cfg.totalThreads);
        for (size_t k = 0; k < work.size(); k++) { jobs[workIdx[k]].ok = work[k].ok; jobs[workIdx[k]].out.swap(work[k].out); }
        c.out.clear();
        std::vector<size_t> dataAt(ents.size());
        for (size_t i = 0; i < ents.size(); i++) {
            if (!jobs[i].ok) return c.fail("Compression failed for " + ents[i].rel);
            Entry& e = ents[i];
            bool store = forceStore[i] || jobs[i].out.size() >= e.size;
            e.method = store ? 0 : 8; e.csize = store ? e.size : jobs[i].out.size(); e.lfh = c.st.dataEnd + c.out.size();
            Buf h; zipLFH(h, e);
            c.out.insert(c.out.end(), h.b.begin(), h.b.end());
            dataAt[i] = c.out.size();
            if (store) c.out.insert(c.out.end(), jobs[i].in, jobs[i].in + e.size);
            else c.out.insert(c.out.end(), jobs[i].out.begin(), jobs[i].out.end());
            std::vector<uint8_t>().swap(jobs[i].out);
        }
        fo.packSize = c.out.size();
        if (!commitData(c)) return false;
        for (size_t i = 0; i < ents.size(); i++) {
            const Entry& e = ents[i]; const uint8_t* src = c.buf.data() + offs[i];
            bool ok = e.method == 0 ? memcmp(c.out.data() + dataAt[i], src, (size_t)e.size) == 0
                                    : inflateVerify(c.out.data() + dataAt[i], (size_t)e.csize, src, (size_t)e.size, true);
            if (!ok) return c.fail("Verification failed for " + e.rel + ". Nothing was deleted.");
        }
    }
    CP("batch-verified");
    Buf rec; rec.u8(R_BATCH); rec.u64(c.st.dataEnd + c.out.size()); rec.u64(fo.packSize); rec.u8(fo.prop); rec.u32((uint32_t)ents.size());
    for (auto& e : ents) putEntry(rec, e);
    if (!c.j.append(rec)) { c.journalBroken = true; return c.fail("Could not write the journal: " + plat::lastError()); }
    CP("batch-journaled");
    c.st.dataEnd += c.out.size(); c.stepOut = 0;
    for (auto& e : ents) c.st.entries.push_back(e);
    c.st.folders.push_back(fo);
    if (!writeIndex(c, false)) return false;
    CP("batch-indexed");
    c.pr.doneBytes = c.stepBase + n; c.pr.filesDone += ents.size();
    if (c.opt.eat) for (auto& e : ents) { deleteSource(c, e); CP("batch-delete"); }
    tick(c, true);
    return true;
}

// Big files: compress one step at a time and release the already-archived part of the source.
static bool processLarge(Ctx& c, const PlanFile& f, bool resuming) {
    plat::File in; bool punch = c.opt.eat && c.holes;
    if (!in.open(f.src, punch ? plat::OpenReadPunch : plat::OpenRead)) {
        if (resuming) return c.fail("Cannot reopen the half-processed file " + f.src + ": " + plat::lastError());
        if (punch && in.open(f.src, plat::OpenRead)) { punch = false; c.warn("No write access, so space is released only at the end: " + f.src); }
        else { c.warn("In use or unreadable, skipped (" + plat::lastError() + "): " + f.src); c.pr.totalBytes -= std::min(c.pr.totalBytes, f.size); if (c.pr.filesTotal) c.pr.filesTotal--; return true; }
    }
    uint64_t size = 0;
    if (!in.size(size)) return c.fail("Cannot read size of " + f.src);
    PartState& p = c.st.part;
    if (!resuming) {
        if (size <= c.cfg.step) { in.close(); std::vector<PlanFile> one(1, f); return processBatch(c, one); }
        p = PartState(); p.active = true;
        p.e.src = f.src; p.e.rel = f.rel; p.e.size = size; p.e.mtime = f.mtime; p.e.attr = f.attr; p.e.z64 = 1; p.e.method = 8;
        p.offset = 0; p.crcState = CRC_INIT_VAL; p.packStart = c.st.dataEnd; p.e.lfh = c.st.dataEnd;
        p.prop = lzma2PropFor(c.cfg.dict);
    } else if (size != p.e.size) {
        return c.fail("The half-processed file changed size and cannot be continued: " + f.src, false);
    } else if (c.opt.format == F7Z && p.prop != lzma2PropFor(c.cfg.dict)) {
        return c.fail("Compression settings changed since this file was started");
    }
    c.pr.current = f.rel;
    try { c.buf.resize((size_t)std::min<uint64_t>(c.cfg.step, size)); } catch (...) { return c.fail("Out of memory"); }
    while (p.offset < size) {
        pollStop(c);
        if (c.stop == 1) return false;
        size_t n = (size_t)std::min<uint64_t>(c.cfg.step, size - p.offset), got = 0;
        bool final = p.offset + n == size, first = p.offset == 0;
        if (!in.readAt(p.offset, c.buf.data(), n, got) || got != n) return c.fail("Read error in " + f.src + ": " + plat::lastError());
        c.stepBase = c.pr.doneBytes; c.stepOut = 0;
        size_t hdrLen = 0; uint8_t prop = p.prop;
        bool stored;
        if (c.opt.format == F7Z) stored = sampleRatio(c.buf.data(), n) > kStoreRatio;
        else if (!first) stored = p.e.method == 0;
        else {
            // A ZIP entry has one method for the whole file, so sample across the whole file before choosing.
            const size_t piece = 32768; std::vector<uint8_t> smp(24 * piece); size_t have = 0, g2 = 0;
            for (int i = 0; i < 24; i++) {
                uint64_t at = (size - piece) * (uint64_t)i / 23;
                if (in.readAt(at, smp.data() + have, piece, g2) && g2 == piece) have += piece;
            }
            stored = sampleRatio(smp.data(), have) > kStoreRatio;
        }
        if (c.opt.format == F7Z) {
            if (stored) { if (!storeLzma2(c.buf.data(), n, final, c.out)) return c.fail("Out of memory"); }
            else {
                if (!lzmaEncode(c, c.buf.data(), n, final, false, c.out, prop)) return false;
                if (prop != p.prop) return c.fail("Compression settings changed since this file was started");
            }
            if (!commitData(c)) return false;
            if (!lzmaVerify(c.out.data(), c.out.size(), prop, c.buf.data(), n, final))
                return c.fail("Verification failed: decompressed data does not match the source. Nothing was released.");
        } else if ((first ? (p.e.method = stored ? 0 : 8) : p.e.method) == 0) {
            // ZIP "stored" entry: the data goes in unchanged.
            c.out.clear();
            if (first) { Buf h; zipLFH(h, p.e); c.out = h.b; hdrLen = c.out.size(); }
            try { c.out.insert(c.out.end(), c.buf.data(), c.buf.data() + n); } catch (...) { return c.fail("Out of memory"); }
            if (!commitData(c)) return false;
            if (memcmp(c.out.data() + hdrLen, c.buf.data(), n) != 0)
                return c.fail("Verification failed: stored data does not match the source. Nothing was released.");
        } else {
            size_t nb = (n + c.cfg.block - 1) / c.cfg.block;
            std::vector<ZJob> jobs(nb);
            for (size_t i = 0; i < nb; i++) {
                size_t a = i * (size_t)c.cfg.block;
                jobs[i].in = c.buf.data() + a; jobs[i].n = std::min<size_t>((size_t)c.cfg.block, n - a);
                jobs[i].flush = (final && i + 1 == nb) ? Z_FINISH : Z_FULL_FLUSH;
            }
            runZJobs(jobs, c.cfg.zLevel, c.cfg.totalThreads);
            c.out.clear();
            if (first) { Buf h; zipLFH(h, p.e); c.out = h.b; hdrLen = c.out.size(); }
            for (auto& j : jobs) { if (!j.ok) return c.fail("Compression failed for " + f.rel); c.out.insert(c.out.end(), j.out.begin(), j.out.end()); std::vector<uint8_t>().swap(j.out); }
            if (!commitData(c)) return false;
            if (!inflateVerify(c.out.data() + hdrLen, c.out.size() - hdrLen, c.buf.data(), n, final))
                return c.fail("Verification failed: decompressed data does not match the source. Nothing was released.");
        }
        CP("part-verified");
        uint32_t crcState = CrcUpdate(p.crcState, c.buf.data(), n);
        Entry e2 = p.e; e2.csize += c.out.size() - hdrLen;
        uint64_t newEnd = c.st.dataEnd + c.out.size();
        if (final && c.opt.format == FZIP) {
            // Fill the real CRC and sizes into the local header written with the first step.
            e2.crc = CRC_GET_DIGEST(crcState);
            Buf h; zipLFH(h, e2);
            if (!c.arc.writeAt(e2.lfh, h.b.data(), h.b.size()) || !c.arc.flush()) return c.fail("Could not update file header: " + plat::lastError());
        }
        Buf rec; rec.u8(R_PART); rec.u64(newEnd); putEntry(rec, e2); rec.u64(p.offset + n); rec.u32(crcState); rec.u64(p.packStart); rec.u8(p.prop);
        if (!c.j.append(rec)) { c.journalBroken = true; return c.fail("Could not write the journal: " + plat::lastError()); }
        CP("part-journaled");
        c.st.dataEnd = newEnd; c.stepOut = 0; p.e = e2; p.crcState = crcState;
        uint64_t prev = p.offset; p.offset += n;
        c.pr.doneBytes = c.stepBase + n;
        if (final) {
            Entry e = p.e; e.crc = CRC_GET_DIGEST(crcState);
            Folder fo; fo.packSize = c.st.dataEnd - p.packStart; fo.prop = p.prop; fo.first = c.st.entries.size(); fo.count = 1;
            c.st.entries.push_back(e); c.st.folders.push_back(fo);
            p = PartState();
            if (!writeIndex(c, false)) return false;
            CP("part-indexed");
            in.close();
            c.pr.filesDone++;
            if (c.opt.eat) {
                uint64_t before = c.pr.reclaimedBytes;
                if (plat::removeFile(e.src)) { c.pr.reclaimedBytes = before + (punch ? size - prev : size); logDeleted(c, e.src, size); }
                else c.warn("Archived but could not be deleted (" + plat::lastError() + "): " + e.src);
            }
            CP("part-deleted");
            tick(c, true);
            return true;
        } else if (punch) {
            uint64_t from = prev & ~((1ull << 20) - 1);
            if (in.punchHole(from, p.offset - from)) c.pr.reclaimedBytes += n;
            else { punch = false; c.warn("This drive would not release space inside " + f.src + "; it will be freed when the file is finished."); }
            CP("part-punched");
        }
        tick(c, true);
    }
    return true;
}

static bool openArchiveNew(Ctx& c) {
    if (!c.arc.create(c.archive, c.opt.split)) return c.fail("Cannot create archive " + c.archive + ": " + plat::lastError(), false);
    if (c.opt.format == F7Z) {
        std::vector<uint8_t> sh = build7zStart(0, 0, 0);
        if (!c.arc.writeAt(0, sh.data(), sh.size()) || !c.arc.flush()) return c.fail("Cannot write archive: " + plat::lastError(), false);
        c.st.dataEnd = 32;
    } else c.st.dataEnd = 0;
    return true;
}

} // namespace

// ---------------------------------------------------------------- public API
std::string journalPathFor(const std::string& archive) { return archive + ".nzj"; }
void discardStaleJournal(const std::string& archive0) {
    std::string archive = plat::absPath(archive0);
    if (ArcFile::exists(archive)) return;
    plat::removeFile(journalPathFor(archive)); plat::removeFile(mirrorPathFor(archive));
}
static bool pathOverlap(const std::string& a, const std::string& b) {
    std::string ka = plat::pathKey(a), kb = plat::pathKey(b);
    auto under = [](const std::string& child, const std::string& par) {
        return child.size() > par.size() && child.compare(0, par.size(), par) == 0 &&
               (child[par.size()] == '/' || child[par.size()] == '\\' || par.back() == '/' || par.back() == '\\');
    };
    return ka == kb || under(ka, kb) || under(kb, ka) || ArcFile::isPartOf(ka, kb) || ArcFile::isPartOf(kb, ka);
}
std::string claimConflict(const std::vector<std::string>& paths, const std::string& ownArchive) {
    for (auto& ji : pendingJobs()) {
        if (!ownArchive.empty() && plat::samePath(ji.archive, ownArchive)) continue;
        std::vector<std::string> claims = ji.opt.inputs;
        claims.push_back(ji.archive); claims.push_back(journalPathFor(ji.archive));
        for (auto& p : paths) for (auto& c : claims)
            if (pathOverlap(p, c))
                return "\"" + plat::baseName(p) + "\" is part of an unfinished compression job (archive " + ji.archive +
                       "), which is not finished yet. Resume, undo or close that job first, using Unfinished jobs.";
    }
    for (auto& xi : pendingExtractions()) {
        if (!ownArchive.empty() && plat::samePath(xi.archive, ownArchive)) continue;
        std::vector<std::string> claims; claims.push_back(xi.archive); claims.push_back(xi.dest);
        for (auto& p : paths) for (auto& c : claims)
            if (pathOverlap(p, c))
                return "\"" + plat::baseName(p) + "\" is part of an unfinished extraction (" + xi.archive + " into " + xi.dest +
                       "). Its files are not complete yet. Finish that extraction first, using Unfinished jobs.";
    }
    return "";
}

bool archiveExists(const std::string& archive) { return ArcFile::exists(plat::absPath(archive)); }
std::string deletionLogPath() { return plat::join(plat::appDataDir(), "deleted-files.log"); }

std::string humanSize(uint64_t n) {
    const char* u[] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)n; int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    char b[64];
    if (i == 0) snprintf(b, sizeof(b), "%llu bytes", (unsigned long long)n);
    else snprintf(b, sizeof(b), v >= 100 ? "%.0f %s" : v >= 10 ? "%.1f %s" : "%.2f %s", v, u[i]);
    return b;
}

Preflight preflight(const Options& opt) {
    Preflight pf;
    crcInit();
    if (opt.inputs.empty()) { pf.error = "Nothing selected."; return pf; }
    if (opt.archive.empty()) { pf.error = "Choose where to save the archive."; return pf; }
    std::string archive = plat::absPath(opt.archive), dir = plat::parentDir(archive);
    plat::Stat st;
    if (!plat::stat(dir, st) || !st.isDir) { pf.error = "The archive folder does not exist: " + dir; return pf; }
    if (ArcFile::exists(archive)) { pf.error = "A file with that archive name already exists. Pick another name."; return pf; }
    {   // Can we actually write here? (Windows blocks some places, such as C:\ itself or protected folders.)
        std::string probe = plat::join(dir, ".nibblezip-write-test.tmp"); plat::File t;
        plat::removeFile(probe);
        if (!t.open(probe, plat::OpenRWCreateNew)) {
            pf.error = "Windows would not let NibbleZip create files in " + dir + " (" + plat::lastError() + "). "
                       "Choose a different folder for the archive. If Controlled folder access is on in Windows Security, allow NibbleZip there.";
            return pf;
        }
        t.close(); plat::removeFile(probe);
    }
    if (opt.split && opt.split < (1u << 20) && !opt.testStep) { pf.error = "Parts must be at least 1 MB."; return pf; }
    Plan plan; std::string err;
    if (!scan(opt.inputs, archive, plan, err, false)) { pf.error = err; return pf; }
    // Never take in anything that belongs to another unfinished job. A file that job is half-way through is
    // already partly hollowed out, and an unfinished archive or journal is still needed to finish that job.
    if (!plan.jobFiles.empty()) {
        pf.error = "The selection contains an unfinished NibbleZip job (" + plat::baseName(plan.jobFiles[0]) + " in " + plat::parentDir(plan.jobFiles[0]) +
                   "). Finish, undo or close that job first, using Unfinished jobs.";
        return pf;
    }
    {
        std::set<std::string> keys;
        for (auto& f : plan.files) keys.insert(plat::pathKey(f.src));
        auto inSelection = [&](const std::string& arc) {
            std::string ak = plat::pathKey(arc);
            if (keys.count(ak)) return true;
            for (auto& k : keys) if (ArcFile::isPartOf(k, ak)) return true;
            return false;
        };
        for (auto& ji : pendingJobs()) {
            if (plat::samePath(ji.archive, archive)) continue;
            if (ji.midFile && keys.count(plat::pathKey(ji.midFileSrc))) {
                pf.error = "\"" + plat::baseName(ji.midFileSrc) + "\" is half-way through another unfinished job (" + ji.archive +
                           "). Part of it is only in that archive. Resume or undo that job first, using Unfinished jobs.";
                return pf;
            }
            if (inSelection(ji.archive)) { pf.error = "The selection contains the archive of an unfinished job (" + ji.archive + "). Finish or close that job first, using Unfinished jobs."; return pf; }
        }
        for (auto& xi : pendingExtractions())
            if (inSelection(xi.archive)) { pf.error = "The selection contains an archive that is part-way through being extracted (" + xi.archive + "). Finish that extraction first, using Unfinished jobs."; return pf; }
    }
    {
        std::vector<std::string> mine;
        for (auto& in : opt.inputs) mine.push_back(plat::absPath(in));
        mine.push_back(archive);
        std::string clash = claimConflict(mine, archive);
        if (!clash.empty()) { pf.error = clash; return pf; }
    }
    pf.totalBytes = plan.total; pf.files = plan.files.size(); pf.dirs = plan.dirs.size(); pf.largest = plan.largest;
    for (auto& s : plan.skipped) if (pf.notes.size() < 20) pf.notes.push_back(s);
    if (plan.files.empty() && plan.dirs.empty()) { pf.error = "Nothing to compress."; return pf; }
    if (!plat::freeSpace(dir, pf.freeBytes)) { pf.error = "Cannot read free space of " + dir; return pf; }
    std::string av = plat::volumeId(dir);
    for (auto& in : opt.inputs) {
        std::string a = plat::absPath(in);
        if (plat::volumeId(a) != av) pf.sameVolume = false;
        if (!plat::supportsHoles(a)) pf.holes = false;
    }
    StepCfg cfg = makeCfg(opt, pf.freeBytes);
    pf.stepBytes = cfg.step;
    const uint64_t margin = 16ull << 20;
    uint64_t idx = 0;
    for (auto& f : plan.files) idx += f.rel.size() * 2 + 64;
    for (auto& d : plan.dirs) idx += d.rel.size() * 2 + 64;
    if (opt.eat && pf.sameVolume) {
        uint64_t unit = std::min(plan.total, cfg.step);
        if (!pf.holes && plan.largest > cfg.step) unit = plan.largest;
        pf.needBytes = worstCase(unit) + idx + margin;
        if (pf.freeBytes < pf.needBytes) {
            pf.error = "Not enough free space to start safely: " + humanSize(pf.needBytes) + " needed, " + humanSize(pf.freeBytes) + " free.";
            return pf;
        }
        if (!pf.holes && plan.largest > cfg.step)
            pf.notes.push_back("This drive cannot release space inside a file while it is being read, so each large file needs room for its compressed copy.");
    } else {
        pf.needBytes = worstCase(plan.total) + idx + margin;
        if (opt.eat && !pf.sameVolume)
            pf.notes.push_back("The archive is on a different drive from the files, so deleting them does not make room for it.");
        if (pf.freeBytes < pf.needBytes)
            pf.notes.push_back("If the data compresses poorly the archive drive may fill up (" + humanSize(pf.freeBytes) + " free). The job would stop safely and can be resumed.");
    }
    pf.ok = true;
    return pf;
}

bool loadJob(const std::string& archive0, JobInfo& info) {
    crcInit();
    std::string archive = plat::absPath(archive0);
    JHeader h; State st; std::vector<uint8_t> bytes;
    if (!loadJournal(archive, h, st, bytes)) return false;
    info = JobInfo();
    info.archive = archive; info.opt.archive = archive; info.opt.format = h.format; info.opt.level = h.level; info.opt.eat = h.eat; info.opt.inputs = h.inputs; info.opt.split = h.split;
    info.midFile = st.part.active; info.midFileName = st.part.e.rel; info.midFileSrc = st.part.e.src; info.filesDone = st.entries.size(); info.packedBytes = st.dataEnd;
    return true;
}

std::vector<JobInfo> pendingJobs() {
    crcInit();
    std::vector<JobInfo> out; std::vector<plat::DirEnt> ents;
    if (!plat::listDir(mirrorDir(), ents)) return out;
    for (auto& e : ents) {
        std::string p = plat::join(mirrorDir(), e.name);
        std::vector<uint8_t> b; JHeader h; State st;
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".nzj") != 0) continue;
        if (!readAll(p, b) || !parseJournal(b, h, st)) continue;
        if (!ArcFile::exists(h.archive)) continue;
        JobInfo ji;
        if (loadJob(h.archive, ji)) out.push_back(ji);
    }
    return out;
}

uint64_t estimateArchiveSize(const std::vector<std::string>& inputs, int format, int level, uint64_t& totalBytes) {
    crcInit();
    totalBytes = 0;
    Plan plan; std::string err;
    if (!scan(inputs, "", plan, err, true)) return 0;
    totalBytes = plan.total;
    if (plan.total == 0) return 0;
    const size_t piece = 1 << 20;
    size_t K = (size_t)std::min<uint64_t>(24, (plan.total + piece - 1) / piece);
    int lv = std::min(3, std::max(0, level));
    static const int zLevels[4] = {3, 6, 9, 9};
    std::vector<uint8_t> buf(piece), out;
    uint64_t in = 0, packed = 0, cum = 0; size_t fi = 0;
    for (size_t k = 0; k < K; k++) {
        uint64_t target = plan.total / K * k;                     // position in all the data laid end to end
        while (fi < plan.files.size() && cum + plan.files[fi].size <= target) { cum += plan.files[fi].size; fi++; }
        if (fi >= plan.files.size()) break;
        const PlanFile& f = plan.files[fi];
        uint64_t off = target - cum; size_t want = (size_t)std::min<uint64_t>(piece, f.size - off), got = 0;
        plat::File fh;
        if (!fh.open(f.src, plat::OpenRead) || !fh.readAt(off, buf.data(), want, got) || got == 0) continue;
        size_t n = got, o = n;
        if (sampleRatio(buf.data(), n) <= kStoreRatio) {
            if (format == F7Z) {
                CLzma2EncHandle h = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
                if (h) {
                    CLzma2EncProps p; Lzma2EncProps_Init(&p);
                    p.lzmaProps.level = lv == 0 ? 1 : 3; p.lzmaProps.dictSize = 1 << 20; p.lzmaProps.reduceSize = n; p.numTotalThreads = 1; p.numBlockThreads_Max = 1;
                    out.clear(); OutVec ov; ov.vt.Write = outWrite; ov.v = &out;
                    if (Lzma2Enc_SetProps(h, &p) == SZ_OK && Lzma2Enc_Encode2(h, &ov.vt, nullptr, nullptr, nullptr, buf.data(), n, nullptr) == SZ_OK) o = std::min(n, out.size());
                    Lzma2Enc_Destroy(h);
                }
            } else {
                ZJob j; j.in = buf.data(); j.n = n; j.flush = Z_FINISH; deflateOne(j, zLevels[lv]);
                if (j.ok) o = std::min(n, j.out.size());
            }
        }
        in += n; packed += o;
    }
    if (in == 0) return 0;
    double ratio = (double)packed / (double)in;
    return (uint64_t)((double)plan.total * ratio) + plan.files.size() * 64 + 1024;
}

Result undoPartial(const std::string& archive0, Callbacks& cb) {
    crcInit();
    cpInit();
    Ctx c; c.cb = &cb;
    c.archive = plat::absPath(archive0); c.arcDir = plat::parentDir(c.archive);
    c.lastTick = std::chrono::steady_clock::now();
    auto finish = [&]() -> Result { c.res.final = c.pr; c.cb->progress(c.pr); return c.res; };
    JHeader jh; std::vector<uint8_t> jbytes;
    if (!loadJournal(c.archive, jh, c.st, jbytes)) { c.fail("No unfinished job was found for this archive.", false); return finish(); }
    if (!c.st.part.active || c.st.partSteps.empty()) { c.fail("No file is half-processed in this job, so there is nothing to put back.", false); return finish(); }
    c.opt.archive = c.archive; c.opt.format = jh.format; c.opt.level = jh.level; c.opt.eat = jh.eat; c.opt.inputs = jh.inputs; c.opt.split = jh.split;
    if (!c.arc.openExisting(c.archive, c.opt.split, false)) { c.fail("Cannot open archive " + c.archive + ": " + plat::lastError()); return finish(); }
    uint64_t asz = 0; c.arc.size(asz);
    if (asz < c.st.dataEnd) { c.fail("The archive is shorter than its journal says. Nothing was changed.", false); return finish(); }
    const Entry e = c.st.part.e;
    plat::File src; uint64_t ssz = 0;
    if (!src.open(e.src, plat::OpenReadPunch) || !src.size(ssz)) { c.fail("Cannot open the original for writing: " + e.src + " (" + plat::lastError() + ")"); return finish(); }
    if (ssz != e.size) { c.fail("The original has changed size, so it cannot be put back safely: " + e.src, false); return finish(); }
    if (!c.j.start(c.archive, jbytes)) { c.fail("Cannot open the journal: " + plat::lastError()); return finish(); }
    size_t lfhLen = 0;
    if (c.opt.format == FZIP) { Buf h; zipLFH(h, e); lfhLen = h.b.size(); }
    const std::vector<PartStep> steps = c.st.partSteps;
    c.pr.totalBytes = 2 * c.st.part.offset; c.pr.filesTotal = 1; c.pr.current = e.rel;
    // Decompresses step k into c.buf.
    auto decode = [&](size_t k) -> bool {
        const PartStep& ps = steps[k];
        size_t pn = (size_t)(ps.endAfter - ps.endBefore), n = (size_t)(ps.offAfter - ps.offBefore), got = 0;
        try { c.out.resize(pn); c.buf.resize(n); } catch (...) { return c.fail("Out of memory"); }
        if (!c.arc.readAt(ps.endBefore, c.out.data(), pn, got) || got != pn) return c.fail("Could not read the archive: " + plat::lastError());
        size_t skip = (c.opt.format == FZIP && k == 0) ? lfhLen : 0;
        if (skip > pn) return c.fail("The archive data for this file is not as expected.");
        bool ok;
        if (c.opt.format == F7Z) ok = lzmaDecodeRun(c.out.data(), pn, c.st.part.prop, c.buf.data(), n);
        else if (e.method == 0) { ok = pn - skip == n; if (ok) memcpy(c.buf.data(), c.out.data() + skip, n); }
        else ok = inflateRun(c.out.data() + skip, pn - skip, c.buf.data(), n);
        if (!ok) return c.fail("The stored part of this file could not be read back correctly. Nothing more was changed; resume the job instead.");
        return true;
    };
    // Pass 1: prove the stored part is intact before touching anything.
    uint32_t crc = CRC_INIT_VAL;
    for (size_t k = 0; k < steps.size(); k++) {
        pollStop(c);
        if (c.stop == 1) { c.res.paused = true; c.res.resumable = true; return finish(); }
        if (!decode(k)) return finish();
        crc = CrcUpdate(crc, c.buf.data(), c.buf.size());
        c.pr.doneBytes += c.buf.size(); tick(c, false);
    }
    if (crc != c.st.part.crcState) { c.fail("The stored part of this file did not pass its check, so nothing was changed. Resume the job instead."); return finish(); }
    // Pass 2: put the steps back, last one first, shrinking the archive as we go.
    bool holes = plat::supportsHoles(e.src);
    for (size_t k = steps.size(); k-- > 0;) {
        const PartStep& ps = steps[k];
        if (!decode(k)) return finish();
        size_t n = c.buf.size(), got = 0; uint64_t fr = 0;
        if (holes && plat::freeSpace(plat::parentDir(e.src), fr) && fr < n + (8ull << 20)) {
            c.fail("Not enough free space to put the next piece back (" + humanSize(n) + " needed, " + humanSize(fr) + " free). Free some space and try again."); return finish();
        }
        uint32_t want = CrcCalc(c.buf.data(), n);
        if (!src.writeAt(ps.offBefore, c.buf.data(), n) || !src.flush()) { c.fail("Writing to the original failed: " + plat::lastError()); return finish(); }
        if (!src.readAt(ps.offBefore, c.buf.data(), n, got) || got != n || CrcCalc(c.buf.data(), n) != want) { c.fail("The original did not read back correctly after writing. The archive was left as it is."); return finish(); }
        CP("undo-written");
        Buf rec; rec.u8(R_UNPART);
        if (!c.j.append(rec)) { c.fail("Could not write the journal: " + plat::lastError()); return finish(); }
        CP("undo-journaled");
        c.st.dataEnd = ps.endBefore; c.st.part.offset = ps.offBefore; c.st.part.crcState = ps.crcBefore; c.st.part.e.csize = ps.csizeBefore;
        c.st.partSteps.pop_back();
        if (c.st.partSteps.empty()) c.st.part = PartState();
        if (!c.arc.truncate(c.st.dataEnd) || !c.arc.flush()) { c.fail("Could not shrink the archive: " + plat::lastError()); return finish(); }
        CP("undo-truncated");
        c.pr.doneBytes += n; tick(c, true);
        pollStop(c);
        if (c.stop == 1 && k > 0) { c.res.paused = true; c.res.resumable = true; return finish(); }
    }
    src.close();
    plat::setMTime(e.src, e.mtime);
    if (c.st.entries.empty()) {
        c.arc.close(); ArcFile::removeAll(c.archive, c.opt.split);
        c.j.destroy();
        c.res.archiveRemoved = true;
    } else {
        if (!writeIndex(c, false)) return finish();
        CP("undo-indexed");
        c.j.destroy(); c.arc.close();
        c.res.partial = true;
    }
    c.pr.filesDone = c.st.entries.size(); c.pr.packedBytes = c.res.archiveRemoved ? 0 : c.st.dataEnd;
    c.res.ok = true;
    return finish();
}

Result run(const Options& opt0, Callbacks& cb) {
    crcInit();
    cpInit();
    Ctx c; c.opt = opt0; c.cb = &cb;
    c.archive = plat::absPath(opt0.archive); c.arcDir = plat::parentDir(c.archive);
    c.lastTick = std::chrono::steady_clock::now();
    auto finish = [&]() -> Result { c.stepOut = 0; c.pr.packedBytes = c.st.dataEnd; c.res.final = c.pr; c.cb->progress(c.pr); return c.res; };

    JHeader jh; std::vector<uint8_t> jbytes; State loaded;
    bool resume = loadJournal(c.archive, jh, loaded, jbytes);
    if (resume) {
        if (!ArcFile::exists(c.archive)) {   // journal left over from an archive that no longer exists
            plat::removeFile(journalPathFor(c.archive)); plat::removeFile(mirrorPathFor(c.archive));
            resume = false; loaded = State();
            // Nothing had been written yet: start that job over with the settings it was given.
            if (c.opt.inputs.empty()) { c.opt.inputs = jh.inputs; c.opt.format = jh.format; c.opt.level = jh.level; c.opt.eat = jh.eat; c.opt.split = jh.split; c.opt.finalizeOnly = false; }
        }
    }
    if (resume) {
        c.st = std::move(loaded);
        c.opt.format = jh.format; c.opt.level = jh.level; c.opt.eat = jh.eat; c.opt.inputs = jh.inputs; c.opt.split = jh.split;
        if (!c.arc.openExisting(c.archive, c.opt.split, false)) { c.fail("Cannot open archive " + c.archive + ": " + plat::lastError()); return finish(); }
        uint64_t sz = 0; c.arc.size(sz);
        if (sz < c.st.dataEnd) { c.fail("The archive is shorter than its journal says. It may have been modified; not continuing.", false); return finish(); }
        if (!c.j.start(c.archive, jbytes)) { c.fail("Cannot open the journal: " + plat::lastError()); return finish(); }
    } else {
        if (c.opt.finalizeOnly) { c.fail("No unfinished job found for this archive.", false); return finish(); }
        Preflight pf = preflight(c.opt);
        if (!pf.ok) { c.fail(pf.error, false); return finish(); }
        // The journal is created before the archive, so a crash in between never leaves an archive without one.
        Buf h; h.u8(R_HEADER); h.u32(JMAGIC); h.u8((uint8_t)c.opt.format); h.u8((uint8_t)c.opt.level); h.u8(c.opt.eat ? 1 : 0); h.str(c.archive);
        h.u32((uint32_t)c.opt.inputs.size());
        for (auto& in : c.opt.inputs) h.str(plat::absPath(in));
        h.u64(c.opt.split);
        if (!c.j.start(c.archive, std::vector<uint8_t>()) || !c.j.append(h)) { c.fail("Cannot create the journal: " + plat::lastError(), false); c.j.destroy(); return finish(); }
        CP("journal-created");
        if (!openArchiveNew(c)) { c.j.destroy(); return finish(); }
        CP("archive-created");
        for (auto& in : c.opt.inputs) in = plat::absPath(in);
    }
    for (auto& e : c.st.entries) c.idxEst += e.rel.size() * 2 + 64;

    if (c.opt.finalizeOnly) {
        if (c.st.part.active) { c.fail("A large file is half-processed (" + c.st.part.e.rel + "). The job has to be resumed; it cannot be closed here.", true); return finish(); }
        if (!writeIndex(c, false)) return finish();
        c.j.destroy(); c.arc.close();
        c.pr.filesDone = c.st.entries.size(); c.res.ok = true; c.res.partial = true;
        return finish();
    }

    Plan plan; std::string err;
    if (!scan(c.opt.inputs, c.archive, plan, err, resume)) { c.fail(err); return finish(); }
    for (auto& s : plan.skipped) c.warn(s);

    c.holes = true;
    for (auto& in : c.opt.inputs) { plat::Stat s; if (plat::stat(in, s) && !plat::supportsHoles(in)) c.holes = false; }
    uint64_t fr = ~0ull; plat::freeSpace(c.arcDir, fr);
    c.cfg = makeCfg(c.opt, fr);

    // Work out what is already inside the archive.
    std::set<std::string> done;
    for (auto& e : c.st.entries) { done.insert(e.src); c.pr.doneBytes += e.size; }
    c.pr.filesDone = c.st.entries.size();
    c.pr.totalBytes = c.pr.doneBytes; c.pr.filesTotal = c.st.entries.size();
    std::vector<PlanFile> todo; const PlanFile* partFile = nullptr;
    for (auto& f : plan.files) {
        if (done.count(f.src)) continue;
        if (c.st.part.active && f.src == c.st.part.e.src) { partFile = &f; c.pr.totalBytes += f.size; c.pr.doneBytes += c.st.part.offset; c.pr.filesTotal++; continue; }
        if (f.size == 0) { bool dup = false; for (auto& x : c.st.empties) if (x.rel == f.rel) dup = true;
            if (!dup) { Entry e; e.src = f.src; e.rel = f.rel; e.mtime = f.mtime; e.attr = f.attr; c.st.empties.push_back(e); } continue; }
        c.pr.totalBytes += f.size; c.pr.filesTotal++;
        todo.push_back(f);
    }
    // Sources that were verified and journaled, but the crash came before they were deleted.
    if (resume && c.opt.eat) {
        uint64_t gone = 0;
        for (auto& e : c.st.entries) { deleteSource(c, e); plat::Stat s; if (!plat::stat(e.src, s)) gone += e.size; }
        if (c.st.part.active && c.holes) gone += c.st.part.offset;
        c.pr.reclaimedBytes = gone;   // total removed over the whole job, not just this session
    }
    tick(c, true);

    bool okSoFar = true;
    if (c.st.part.active) {
        if (!partFile) { c.fail("The half-processed file is missing: " + c.st.part.e.src, false); return finish(); }
        okSoFar = processLarge(c, *partFile, true);
    }
    size_t i = 0;
    while (okSoFar && i < todo.size()) {
        pollStop(c);
        if (c.stop) break;
        if (todo[i].size > c.cfg.step) { okSoFar = processLarge(c, todo[i], false); i++; continue; }
        std::vector<PlanFile> batch; uint64_t sum = 0;
        while (i < todo.size() && todo[i].size <= c.cfg.step && (batch.empty() || sum + todo[i].size <= c.cfg.step)) { sum += todo[i].size; batch.push_back(todo[i]); i++; }
        okSoFar = processBatch(c, batch);
    }
    if (!okSoFar) {
        if (c.res.error.empty()) { c.res.paused = true; c.res.resumable = true; }
        restoreIndex(c);
        return finish();
    }
    if (c.stop == 1) { c.res.paused = true; c.res.resumable = true; return finish(); }
    if (c.stop == 2 && i < todo.size()) {
        if (!writeIndex(c, false)) return finish();
        c.j.destroy(); c.arc.close();
        c.res.ok = true; c.res.partial = true;
        return finish();
    }

    // Finalize: record folders and empty files, write the final index, then clean up.
    {
        std::set<std::string> have;
        for (auto& e : c.st.empties) have.insert(e.rel);
        for (auto& d : plan.dirs) {
            if (have.count(d.rel)) continue;
            Entry e; e.src = d.src; e.rel = d.rel; e.mtime = d.mtime; e.attr = d.attr | 0x10; e.isDir = 1;
            c.st.empties.push_back(e); have.insert(d.rel);
        }
    }
    Buf rec; rec.u8(R_EMPTY); rec.u32((uint32_t)c.st.empties.size());
    for (auto& e : c.st.empties) putEntry(rec, e);
    // De-duplicate on replay is by rel, so journaling the full list again after a crash is harmless.
    {
        std::vector<Entry> uniq; std::set<std::string> seen;
        for (auto& e : c.st.empties) if (seen.insert(e.rel).second) uniq.push_back(e);
        c.st.empties.swap(uniq);
    }
    if (!c.j.append(rec)) { c.fail("Could not write the journal: " + plat::lastError()); return finish(); }
    CP("final-journaled");
    if (!writeIndex(c, true)) return finish();
    CP("final-indexed");
    if (c.opt.eat) {
        for (auto& e : c.st.empties) if (!e.isDir) {
            plat::Stat s;
            if (plat::stat(e.src, s) && s.size == 0) { if (plat::removeFile(e.src)) logDeleted(c, e.src, 0); else c.warn("Could not delete empty file: " + e.src); }
        }
        CP("final-empties-deleted");
        for (size_t k = c.st.empties.size(); k-- > 0;) if (c.st.empties[k].isDir) plat::removeDir(c.st.empties[k].src);
        size_t left = 0;
        for (auto& e : c.st.empties) if (e.isDir) { plat::Stat s; if (plat::stat(e.src, s)) left++; }
        if (left) c.warn(std::to_string(left) + " folder(s) were not removed because they still contain files that were skipped.");
    }
    CP("final-cleaned");
    c.arc.close();
    c.j.destroy();
    c.res.ok = true;
    return finish();
}

} // namespace nz
