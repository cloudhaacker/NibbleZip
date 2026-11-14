// Archive checker: reads a finished 7z or ZIP from disk, decompresses everything and compares checksums.
#include "engine.h"
#include "platform.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "7z.h"
#include "7zCrc.h"
#include "Alloc.h"
#include "Lzma2Dec.h"
#include <zlib.h>

namespace nz {
namespace {

struct VCtx {
    plat::File f; uint64_t size = 0; Callbacks* cb = nullptr; Progress pr; VerifyResult res; int ticks = 0;
    bool fail(const std::string& m) { if (res.error.empty()) res.error = m; return false; }
    bool step(uint64_t done) {
        pr.doneBytes = done;
        if ((++ticks & 7) == 0 || done >= pr.totalBytes) cb->progress(pr);
        if (cb->poll() == 1) { res.cancelled = true; return false; }
        return true;
    }
};

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static std::string utf8From16(const uint16_t* s, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

// ---------------------------------------------------------------- 7z
struct SeekIn { ISeekInStream vt; plat::File* f; uint64_t pos; uint64_t size; };
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

static bool verify7z(VCtx& c) {
    SeekIn si; si.vt.Read = siRead; si.vt.Seek = siSeek; si.f = &c.f; si.pos = 0; si.size = c.size;
    CLookToRead2 look; LookToRead2_CreateVTable(&look, False);
    std::vector<uint8_t> lookBuf(1 << 18);
    look.buf = lookBuf.data(); look.bufSize = lookBuf.size(); look.realStream = &si.vt; LookToRead2_INIT(&look)
    CSzArEx db; SzArEx_Init(&db);
    SRes r = SzArEx_Open(&db, &look.vt, &g_Alloc, &g_Alloc);
    if (r != SZ_OK) {
        SzArEx_Free(&db, &g_Alloc);
        if (r == SZ_ERROR_UNSUPPORTED) return c.fail("This archive uses features the checker does not handle (for example encryption). Use 7-Zip's Test instead.");
        return c.fail(r == SZ_ERROR_NO_ARCHIVE ? "This is not a 7z archive, or its header is damaged." : "The archive index is damaged and cannot be read.");
    }
    bool ok = true;
    std::vector<uint8_t> in(1 << 20), out(1 << 20);
    std::vector<uint16_t> nameBuf;
    auto nameOf = [&](UInt32 i) { size_t n = SzArEx_GetFileNameUtf16(&db, i, nullptr); nameBuf.resize(n + 1); SzArEx_GetFileNameUtf16(&db, i, nameBuf.data()); return utf8From16(nameBuf.data(), n); };
    uint64_t packDoneBase = 0;
    for (UInt32 fi = 0; ok && fi < db.db.NumFolders; fi++) {
        CSzFolder fo; CSzData sd;
        sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fi];
        sd.Size = db.db.FoCodersOffsets[fi + 1] - db.db.FoCodersOffsets[fi];
        if (SzGetNextFolderItem(&fo, &sd) != SZ_OK || fo.NumCoders != 1 || fo.NumPackStreams != 1 ||
            !(fo.Coders[0].MethodID == 0x21 || fo.Coders[0].MethodID == 0) || (fo.Coders[0].MethodID == 0x21 && fo.Coders[0].PropsSize != 1)) {
            ok = c.fail("This archive uses a compression method the checker does not handle (it was probably made by another program). Use 7-Zip's Test instead.");
            break;
        }
        bool lzma2 = fo.Coders[0].MethodID == 0x21;
        UInt32 psi = db.db.FoStartPackStreamIndex[fi];
        uint64_t packPos = db.dataPos + db.db.PackPositions[psi], packSize = db.db.PackPositions[psi + 1] - db.db.PackPositions[psi];
        uint64_t unpackTotal = SzAr_GetFolderUnpackSize(&db.db, fi);
        // files of this folder, in order
        std::vector<UInt32> files;
        for (UInt32 i = db.FolderToFile[fi]; i < db.NumFiles && (fi + 1 >= db.db.NumFolders || i < db.FolderToFile[fi + 1]); i++)
            if (db.FileToFolder[i] == fi) files.push_back(i);
        size_t cur = 0; uint64_t left = 0; uint32_t crc = CRC_INIT_VAL; uint64_t produced = 0;
        auto startFile = [&]() {
            while (cur < files.size()) {
                left = db.UnpackPositions[files[cur] + 1] - db.UnpackPositions[files[cur]]; crc = CRC_INIT_VAL;
                c.pr.current = nameOf(files[cur]);
                if (left) return;
                cur++; c.res.files++;
            }
        };
        auto feed = [&](const uint8_t* p, size_t n) -> bool {
            produced += n; c.res.bytes += n;
            while (n) {
                if (cur >= files.size()) return c.fail("The archive holds more data than its index lists.");
                size_t t = (size_t)std::min<uint64_t>(n, left);
                crc = CrcUpdate(crc, p, t); p += t; n -= t; left -= t;
                if (left == 0) {
                    UInt32 idx = files[cur];
                    if (SzBitWithVals_Check(&db.CRCs, idx) && CRC_GET_DIGEST(crc) != db.CRCs.Vals[idx]) return c.fail("Damaged file (checksum mismatch): " + nameOf(idx));
                    cur++; c.res.files++; startFile();
                }
            }
            return true;
        };
        startFile();
        CLzma2Dec dec; Lzma2Dec_CONSTRUCT(&dec);
        if (lzma2) {
            if (Lzma2Dec_Allocate(&dec, db.db.CodersData[db.db.FoCodersOffsets[fi] + fo.Coders[0].PropsOffset], &g_Alloc) != SZ_OK) { ok = c.fail("Not enough memory to check this archive."); break; }
            Lzma2Dec_Init(&dec);
        }
        uint64_t pdone = 0; bool finished = !lzma2;
        while (ok && pdone < packSize) {
            size_t want = (size_t)std::min<uint64_t>(in.size(), packSize - pdone), got = 0;
            if (!c.f.readAt(packPos + pdone, in.data(), want, got) || got != want) { ok = c.fail("Could not read the archive: " + plat::lastError()); break; }
            if (!lzma2) { ok = feed(in.data(), got); }
            else {
                size_t sp = 0;
                for (;;) {
                    SizeT dl = out.size(), sl = got - sp; ELzmaStatus st = LZMA_STATUS_NOT_SPECIFIED;
                    SRes dr = Lzma2Dec_DecodeToBuf(&dec, out.data(), &dl, in.data() + sp, &sl, LZMA_FINISH_ANY, &st);
                    if (dr != SZ_OK) { ok = c.fail("Damaged data in the archive near: " + c.pr.current); break; }
                    sp += sl;
                    if (dl && !feed(out.data(), dl)) { ok = false; break; }
                    if (st == LZMA_STATUS_FINISHED_WITH_MARK) { finished = true; break; }
                    if (dl == 0 && sl == 0) break;
                    if (sp >= got && dl < out.size()) break;
                }
            }
            pdone += got;
            if (ok && !c.step(packDoneBase + pdone)) ok = false;
        }
        if (lzma2) Lzma2Dec_Free(&dec, &g_Alloc);
        if (ok && (produced != unpackTotal || cur < files.size() || !finished)) ok = c.fail("The archive data ends early or does not match its index.");
        packDoneBase += packSize;
    }
    if (ok) for (UInt32 i = 0; i < db.NumFiles; i++) if (db.FileToFolder[i] == (UInt32)-1 && !SzArEx_IsDir(&db, i)) c.res.files++;
    SzArEx_Free(&db, &g_Alloc);
    return ok;
}

// ---------------------------------------------------------------- ZIP
static bool verifyZip(VCtx& c) {
    size_t tail = (size_t)std::min<uint64_t>(c.size, 65557 + 20), got = 0;
    std::vector<uint8_t> t(tail);
    if (!c.f.readAt(c.size - tail, t.data(), tail, got) || got != tail) return c.fail("Could not read the archive: " + plat::lastError());
    size_t e = (size_t)-1;
    for (size_t i = tail >= 22 ? tail - 22 : 0; tail >= 22; i--) { if (rd32(&t[i]) == 0x06054b50) { e = i; break; } if (i == 0) break; }
    if (e == (size_t)-1) return c.fail("This is not a ZIP archive, or its index is missing (the job may not have been finished).");
    uint64_t n = rd16(&t[e + 10]), cdSize = rd32(&t[e + 12]), cdOff = rd32(&t[e + 16]);
    if (e >= 20 && rd32(&t[e - 20]) == 0x07064b50) {
        uint64_t zpos = rd64(&t[e - 20 + 8]); uint8_t z[56];
        if (!c.f.readAt(zpos, z, 56, got) || got != 56 || rd32(z) != 0x06064b50) return c.fail("The ZIP64 index is damaged.");
        n = rd64(z + 32); cdSize = rd64(z + 40); cdOff = rd64(z + 48);
    }
    if (cdOff + cdSize > c.size || cdSize > (1ull << 31)) return c.fail("The ZIP index is damaged.");
    std::vector<uint8_t> cd((size_t)cdSize);
    if (cdSize && (!c.f.readAt(cdOff, cd.data(), cd.size(), got) || got != cd.size())) return c.fail("Could not read the archive: " + plat::lastError());
    std::vector<uint8_t> in(1 << 20), out(1 << 20);
    size_t p = 0; uint64_t doneBase = 0;
    for (uint64_t k = 0; k < n; k++) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return c.fail("The ZIP index is damaged.");
        uint16_t flags = rd16(&cd[p + 8]), method = rd16(&cd[p + 10]);
        uint32_t crcWant = rd32(&cd[p + 16]);
        uint64_t csize = rd32(&cd[p + 20]), usize = rd32(&cd[p + 24]), off = rd32(&cd[p + 42]);
        size_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
        if (p + 46 + nl + xl + cl > cd.size()) return c.fail("The ZIP index is damaged.");
        std::string name((const char*)&cd[p + 46], nl);
        const uint8_t* x = &cd[p + 46 + nl]; size_t xp = 0;
        while (xp + 4 <= xl) {
            uint16_t tag = rd16(x + xp), len = rd16(x + xp + 2); const uint8_t* d = x + xp + 4; size_t dp = 0;
            if (xp + 4 + len > xl) break;
            if (tag == 1) {
                if (usize == 0xFFFFFFFFu && dp + 8 <= len) { usize = rd64(d + dp); dp += 8; }
                if (csize == 0xFFFFFFFFu && dp + 8 <= len) { csize = rd64(d + dp); dp += 8; }
                if (off == 0xFFFFFFFFu && dp + 8 <= len) { off = rd64(d + dp); dp += 8; }
            }
            xp += 4 + len;
        }
        p += 46 + nl + xl + cl;
        c.pr.current = name;
        if (flags & 1) return c.fail("This archive is encrypted; the checker cannot test it. Use 7-Zip's Test instead.");
        if (method != 0 && method != 8) return c.fail("This archive uses a compression method the checker does not handle. Use 7-Zip's Test instead.");
        uint8_t lh[30];
        if (!c.f.readAt(off, lh, 30, got) || got != 30 || rd32(lh) != 0x04034b50) return c.fail("Damaged entry header: " + name);
        uint64_t dpos = off + 30 + rd16(lh + 26) + rd16(lh + 28);
        if (dpos + csize > c.size) return c.fail("Entry runs past the end of the archive: " + name);
        uint32_t crc = CRC_INIT_VAL; uint64_t produced = 0, pdone = 0; bool ended = method == 0;
        z_stream z; memset(&z, 0, sizeof(z));
        if (method == 8 && inflateInit2(&z, -15) != Z_OK) return c.fail("Not enough memory.");
        bool ok = true;
        while (ok && pdone < csize) {
            size_t want = (size_t)std::min<uint64_t>(in.size(), csize - pdone);
            if (!c.f.readAt(dpos + pdone, in.data(), want, got) || got != want) { ok = c.fail("Could not read the archive: " + plat::lastError()); break; }
            if (method == 0) { crc = CrcUpdate(crc, in.data(), got); produced += got; }
            else {
                z.next_in = in.data(); z.avail_in = (uInt)got;
                while (z.avail_in && !ended) {
                    z.next_out = out.data(); z.avail_out = (uInt)out.size();
                    int zr = inflate(&z, Z_NO_FLUSH);
                    size_t o = out.size() - z.avail_out;
                    crc = CrcUpdate(crc, out.data(), o); produced += o;
                    if (zr == Z_STREAM_END) ended = true;
                    else if (zr != Z_OK && zr != Z_BUF_ERROR) { ok = c.fail("Damaged data in: " + name); break; }
                    else if (o == 0 && z.avail_in == 0) break;
                }
            }
            pdone += got;
            if (ok && !c.step(doneBase + pdone)) ok = false;
        }
        if (method == 8) inflateEnd(&z);
        if (!ok) return false;
        if (!ended && csize) return c.fail("Data ends early in: " + name);
        if (produced != usize || CRC_GET_DIGEST(crc) != crcWant) return c.fail("Damaged file (checksum mismatch): " + name);
        c.res.bytes += produced;
        if (name.empty() || name.back() != '/') c.res.files++;
        doneBase += csize;
    }
    return true;
}

} // namespace

VerifyResult verifyArchive(const std::string& archive0, Callbacks& cb) {
    static bool crcReady = false;
    if (!crcReady) { CrcGenerateTable(); crcReady = true; }
    VCtx c; c.cb = &cb;
    std::string archive = plat::absPath(archive0);
    plat::Stat st;
    if (plat::stat(journalPathFor(archive), st)) { c.fail("This archive belongs to an unfinished job. Resume or close that job first."); return c.res; }
    if (!c.f.open(archive, plat::OpenRead) || !c.f.size(c.size)) { c.fail("Cannot open the archive: " + plat::lastError()); return c.res; }
    uint8_t sig[6] = {0}; size_t got = 0;
    c.f.readAt(0, sig, 6, got);
    c.pr.totalBytes = c.size; c.pr.filesTotal = 0;
    static const uint8_t k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    bool ok = (got == 6 && !memcmp(sig, k7z, 6)) ? verify7z(c) : verifyZip(c);
    c.res.ok = ok && !c.res.cancelled && c.res.error.empty();
    return c.res;
}

} // namespace nz
