// Pieces shared by the compressor (engine.cpp) and the extractor (extract.cpp):
// serialisation helpers, crash-injection checkpoints for the tests, and the two-copy journal.
#pragma once
#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "7zTypes.h"
#include "7zCrc.h"

namespace nz {
namespace detail {

inline int g_cpN = -1, g_cpCount = 0;
inline void cpInit() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = getenv("NIBBLEZIP_CRASH_AT")) { g_cpN = atoi(e); g_cpCount = 0; }
}
// Crash-injection checkpoints, used by the test-suite to kill the process at every stage.
inline void CP(const char* name) {
    if (g_cpN < 0) return;
    if (++g_cpCount == g_cpN) { fprintf(stderr, "CRASH-INJECT %d (%s)\n", g_cpCount, name); fflush(stderr); std::_Exit(77); }
}

struct Buf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void bytes(const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); }
    void str(const std::string& s) { u32((uint32_t)s.size()); bytes(s.data(), s.size()); }
};
struct Rd {
    const uint8_t* p; size_t n; size_t pos = 0; bool bad = false;
    Rd(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    uint8_t u8() { if (pos + 1 > n) { bad = true; return 0; } return p[pos++]; }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
    uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    uint64_t u64() { uint64_t a = u32(); return a | ((uint64_t)u32() << 32); }
    std::string str() { uint32_t l = u32(); if (bad || pos + l > n) { bad = true; return ""; } std::string s((const char*)p + pos, l); pos += l; return s; }
};

inline void crcInit() { static bool done = false; if (!done) { CrcGenerateTable(); done = true; } }

inline bool readAll(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    plat::File f;
    if (!f.open(path, plat::OpenRead)) return false;
    uint64_t sz = 0; if (!f.size(sz)) return false;
    out.resize((size_t)sz); size_t got = 0;
    if (sz && !f.readAt(0, out.data(), out.size(), got)) return false;
    out.resize(got);
    return true;
}

inline std::string mirrorDir() { return plat::join(plat::appDataDir(), "journals"); }
// ext is ".nzj" for compression jobs and ".nzx" for extraction jobs.
inline std::string mirrorPathFor(const std::string& archive, const char* ext = ".nzj") {
    std::string k = plat::pathKey(archive);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : k) { h ^= c; h *= 1099511628211ull; }
    char buf[40]; snprintf(buf, sizeof(buf), "%016llx%s", (unsigned long long)h, ext);
    return plat::join(mirrorDir(), buf);
}

// Journal is written twice: next to the archive and in the app-data folder.
struct Journal {
    plat::File a, b; uint64_t pos = 0; std::string pa, pb; bool haveB = false;
    bool start(const std::string& archive, const std::vector<uint8_t>& existing, const char* ext = ".nzj") {
        pa = archive + ext; pb = mirrorPathFor(archive, ext);
        if (!a.open(pa, plat::OpenRWAlways)) return false;
        plat::makeDirs(mirrorDir());
        haveB = b.open(pb, plat::OpenRWAlways);
        if (!existing.empty() && !a.writeAt(0, existing.data(), existing.size())) return false;
        if (!a.truncate(existing.size()) || !a.flush()) return false;
        if (haveB) haveB = (existing.empty() || b.writeAt(0, existing.data(), existing.size())) && b.truncate(existing.size()) && b.flush();
        pos = existing.size();
        return true;
    }
    bool append(const Buf& payload) {
        Buf fr; fr.u32((uint32_t)payload.b.size()); fr.u32(CrcCalc(payload.b.data(), payload.b.size()));
        fr.bytes(payload.b.data(), payload.b.size());
        if (!a.writeAt(pos, fr.b.data(), fr.b.size()) || !a.flush()) return false;
        if (haveB) haveB = b.writeAt(pos, fr.b.data(), fr.b.size()) && b.flush();
        pos += fr.b.size();
        return true;
    }
    void destroy() { a.close(); b.close(); plat::removeFile(pa); plat::removeFile(pb); }
};

} // namespace detail
} // namespace nz
