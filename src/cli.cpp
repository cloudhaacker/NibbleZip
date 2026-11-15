// Command-line front end (also used by the automated tests).
#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct CliCb : nz::Callbacks {
    uint64_t pauseAfter = 0, finishAfter = 0; bool quiet = false; nz::Progress last;
    void progress(const nz::Progress& p) override {
        last = p;
        if (quiet) return;
        fprintf(stderr, "\r%5.1f%%  archive %s  reclaimed %s   ", p.totalBytes ? 100.0 * p.doneBytes / p.totalBytes : 100.0,
                nz::humanSize(p.packedBytes).c_str(), nz::humanSize(p.reclaimedBytes).c_str());
    }
    int poll() override {
        if (pauseAfter && last.doneBytes >= pauseAfter) return 1;
        if (finishAfter && last.doneBytes >= finishAfter) return 2;
        return 0;
    }
};

static int usage() {
    fprintf(stderr,
        "NibbleZip - compress and reclaim space as it goes\n"
        "  nibblezip-cli pack [-f 7z|zip] [-l 0..3] [--keep] -o ARCHIVE INPUT...\n"
        "  nibblezip-cli pack --each item|file [--outdir DIR] [-f 7z|zip] [-l 0..3] [--keep] INPUT...\n"
        "  nibblezip-cli check [-f 7z|zip] [-l 0..3] [--keep] -o ARCHIVE INPUT...\n"
        "  nibblezip-cli resume ARCHIVE [--finalize]\n"
        "  nibblezip-cli verify ARCHIVE\n"
        "  nibblezip-cli pending\n");
    return 2;
}

#ifdef _WIN32
#include <windows.h>
static int realMain(int argc, char** argv);
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> a; std::vector<char*> p;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        a.push_back(s);
    }
    for (auto& s : a) p.push_back(&s[0]);
    SetConsoleOutputCP(CP_UTF8);
    return realMain(argc, p.data());
}
static int realMain(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    nz::Options o; CliCb cb; int each = nz::OutSingle; std::string outDir;
    if (cmd == "pending") {
        for (auto& j : nz::pendingJobs()) printf("%s  (%llu files done%s)\n", j.archive.c_str(), (unsigned long long)j.filesDone, j.midFile ? ", mid-file" : "");
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-f" && i + 1 < argc) o.format = !strcmp(argv[++i], "zip") ? nz::FZIP : nz::F7Z;
        else if (a == "-l" && i + 1 < argc) o.level = atoi(argv[++i]);
        else if (a == "-o" && i + 1 < argc) o.archive = argv[++i];
        else if (a == "--keep") o.eat = false;
        else if (a == "--split" && i + 1 < argc) o.split = strtoull(argv[++i], nullptr, 10);
        else if (a == "--each" && i + 1 < argc) each = !strcmp(argv[++i], "file") ? nz::OutPerFile : nz::OutPerItem;
        else if (a == "--outdir" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--finalize") o.finalizeOnly = true;
        else if (a == "--quiet") cb.quiet = true;
        else if (a == "--step" && i + 1 < argc) o.testStep = strtoull(argv[++i], nullptr, 10);
        else if (a == "--pause-after" && i + 1 < argc) cb.pauseAfter = strtoull(argv[++i], nullptr, 10);
        else if (a == "--finish-after" && i + 1 < argc) cb.finishAfter = strtoull(argv[++i], nullptr, 10);
        else if ((cmd == "resume" || cmd == "verify" || cmd == "undo") && o.archive.empty()) o.archive = a;
        else o.inputs.push_back(a);
    }
    if (cmd == "estimate") o.archive = "-";
    if (cmd == "pack" && each != nz::OutSingle) {
        nz::BatchPlan plan = nz::planBatch(o, each, outDir);
        for (auto& n : plan.notes) fprintf(stderr, "note: %s\n", n.c_str());
        if (!plan.error.empty()) { fprintf(stderr, "Cannot start: %s\n", plan.error.c_str()); return 1; }
        nz::BatchResult b = nz::runBatch(plan.jobs, cb);
        if (!cb.quiet) fprintf(stderr, "\n");
        for (auto& w : b.r.warnings) fprintf(stderr, "warning: %s\n", w.c_str());
        fprintf(stderr, "%llu of %llu archives made, %llu skipped\n", (unsigned long long)b.done, (unsigned long long)b.total, (unsigned long long)b.skipped);
        if (b.r.paused) { fprintf(stderr, "Paused. Run the same command again to continue.\n"); return 3; }
        if (!b.r.ok) { fprintf(stderr, "Error: %s\n", b.r.error.c_str()); return 1; }
        return 0;
    }
    if (o.archive.empty()) return usage();
    if (cmd == "undo") {
        nz::Result u = nz::undoPartial(o.archive, cb);
        if (!cb.quiet) fprintf(stderr, "\n");
        if (u.ok) { fprintf(stderr, "The file was put back. %s\n", u.archiveRemoved ? "The archive held nothing else and was removed." : "The archive was closed with what it already held."); return 0; }
        if (u.paused) { fprintf(stderr, "Paused.\n"); return 3; }
        fprintf(stderr, "Error: %s\n", u.error.c_str()); return 1;
    }
    if (cmd == "estimate") {
        uint64_t total = 0, est = nz::estimateArchiveSize(o.inputs, o.format, o.level, total);
        printf("%llu %llu\n", (unsigned long long)total, (unsigned long long)est); return 0;
    }
    if (cmd == "verify") {
        nz::VerifyResult v = nz::verifyArchive(o.archive, cb);
        if (!cb.quiet) fprintf(stderr, "\n");
        if (v.ok) { fprintf(stderr, "Archive is OK: %llu files, %s checked.\n", (unsigned long long)v.files, nz::humanSize(v.bytes).c_str()); return 0; }
        fprintf(stderr, "%s\n", v.cancelled ? "Cancelled." : ("PROBLEM: " + v.error).c_str());
        return 1;
    }
    if (cmd == "check" || cmd == "pack") {
        nz::JobInfo ji;
        if (cmd == "check" || !nz::loadJob(o.archive, ji)) {
            nz::Preflight pf = nz::preflight(o);
            if (!pf.ok) { fprintf(stderr, "Cannot start: %s\n", pf.error.c_str()); return 1; }
            fprintf(stderr, "%llu files, %s. Free: %s, needed to start: %s.\n", (unsigned long long)pf.files, nz::humanSize(pf.totalBytes).c_str(),
                    nz::humanSize(pf.freeBytes).c_str(), nz::humanSize(pf.needBytes).c_str());
            for (auto& n : pf.notes) fprintf(stderr, "note: %s\n", n.c_str());
            if (cmd == "check") return 0;
        }
    } else if (cmd != "resume") return usage();
    nz::Result r = nz::run(o, cb);
    if (!cb.quiet) fprintf(stderr, "\n");
    for (auto& w : r.warnings) fprintf(stderr, "warning: %s\n", w.c_str());
    if (r.ok) { fprintf(stderr, "%s %llu files, archive %s, reclaimed %s\n", r.partial ? "Closed early." : "Done.", (unsigned long long)r.final.filesDone,
                        nz::humanSize(r.final.packedBytes).c_str(), nz::humanSize(r.final.reclaimedBytes).c_str()); return 0; }
    if (r.paused) { fprintf(stderr, "Paused. Run 'resume' to continue.\n"); return 3; }
    fprintf(stderr, "Error: %s%s\n", r.error.c_str(), r.resumable ? " (job can be resumed)" : "");
    return 1;
}
