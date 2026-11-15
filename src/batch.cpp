// Making several archives in one go: one per selected item, or one per file.
#include "engine.h"
#include "platform.h"

#include <algorithm>
#include <map>
#include <set>

namespace nz {
namespace {

struct Cand { std::string src, destDir, stem, full; uint64_t bytes = 0; bool isDir = false; };

static std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
static bool endsWith(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && lower(s).compare(s.size() - suf.size(), suf.size(), suf) == 0; }
static std::string stemOf(const std::string& name) { size_t d = name.find_last_of('.'); return (d == std::string::npos || d == 0) ? name : name.substr(0, d); }

static uint64_t dirBytes(const std::string& dir) {
    std::vector<plat::DirEnt> ents; uint64_t n = 0;
    if (!plat::listDir(dir, ents)) return 0;
    for (auto& e : ents) { if (e.st.isLink) continue; n += e.st.isDir ? dirBytes(plat::join(dir, e.name)) : e.st.size; }
    return n;
}
static void walkFiles(const std::string& dir, const std::string& destDir, bool mirror, const std::string& ext, std::vector<Cand>& out, uint64_t& skippedSameType) {
    std::vector<plat::DirEnt> ents;
    if (!plat::listDir(dir, ents)) return;
    for (auto& e : ents) {
        std::string p = plat::join(dir, e.name);
        if (e.st.isLink) continue;
        if (e.st.isDir) { walkFiles(p, mirror ? plat::join(destDir, e.name) : p, mirror, ext, out, skippedSameType); continue; }
        if (endsWith(e.name, ".nzj")) continue;
        if (endsWith(e.name, ext)) { skippedSameType++; continue; }
        Cand c; c.src = p; c.destDir = destDir; c.full = e.name; c.stem = stemOf(e.name); c.bytes = e.st.size;
        out.push_back(std::move(c));
    }
}

// Finds journals of unfinished jobs lying next to archives in a folder.
static void findJournals(const std::string& dir, bool recursive, std::vector<std::string>& archives) {
    std::vector<plat::DirEnt> ents;
    if (!plat::listDir(dir, ents)) return;
    for (auto& e : ents) {
        if (e.st.isLink) continue;
        std::string p = plat::join(dir, e.name);
        if (e.st.isDir) { if (recursive) findJournals(p, true, archives); }
        else if (endsWith(e.name, ".nzj")) archives.push_back(p.substr(0, p.size() - 4));
    }
}

struct Wrap : Callbacks {
    Callbacks* cb = nullptr; Progress acc; uint64_t total = 0, idx = 0, count = 0; int last = 0;
    void progress(const Progress& p) override {
        Progress q;
        q.totalBytes = total; q.doneBytes = std::min(total, acc.doneBytes + p.doneBytes);
        q.packedBytes = acc.packedBytes + p.packedBytes; q.reclaimedBytes = acc.reclaimedBytes + p.reclaimedBytes;
        q.filesDone = acc.filesDone + p.filesDone; q.filesTotal = acc.filesDone + p.filesTotal;
        q.jobIndex = idx; q.jobCount = count; q.current = p.current;
        cb->progress(q);
    }
    int poll() override { last = cb->poll(); return last; }
};

} // namespace

BatchPlan planBatch(const Options& base, int mode, const std::string& outDir0) {
    BatchPlan plan;
    const std::string ext = base.format == FZIP ? ".zip" : ".7z";
    std::string outDir;
    if (!outDir0.empty()) {
        outDir = plat::absPath(outDir0); plat::Stat st;
        if (!plat::stat(outDir, st) || !st.isDir) { plan.error = "The output folder does not exist: " + outDir; return plan; }
    }
    if (base.inputs.empty()) { plan.error = "Nothing selected."; return plan; }
    std::vector<Cand> cands; uint64_t sameType = 0;
    for (auto& in0 : base.inputs) {
        std::string in = plat::absPath(in0), name = plat::baseName(in); plat::Stat st;
        if (!plat::stat(in, st) || st.isLink || name.empty()) continue;
        if (st.isDir && mode == OutPerFile) {
            walkFiles(in, outDir.empty() ? in : plat::join(outDir, name), !outDir.empty(), ext, cands, sameType);
        } else {
            if (!st.isDir && endsWith(name, ext)) { sameType++; continue; }
            if (!st.isDir && endsWith(name, ".nzj")) continue;
            Cand c; c.src = in; c.isDir = st.isDir; c.full = name; c.stem = st.isDir ? name : stemOf(name);
            c.destDir = outDir.empty() ? plat::parentDir(in) : outDir; c.bytes = st.isDir ? dirBytes(in) : st.size;
            cands.push_back(std::move(c));
        }
    }
    if (sameType) plan.notes.push_back(std::to_string(sameType) + " file(s) that are already " + ext + " archives will be left alone.");
    // Unfinished jobs from an earlier run. An original that already belongs to one must continue in that
    // same archive and never be started again under a new name (it may be partly consumed).
    std::map<std::string, JobInfo> pendingByInput; std::vector<JobInfo> pending;
    {
        std::vector<std::string> arcs; std::set<std::string> seen;
        if (!outDir.empty()) findJournals(outDir, true, arcs);
        for (auto& in0 : base.inputs) {
            std::string in = plat::absPath(in0); plat::Stat st;
            findJournals(plat::parentDir(in), false, arcs);
            if (plat::stat(in, st) && st.isDir && !st.isLink) findJournals(in, true, arcs);
        }
        for (auto& ji : pendingJobs()) if (seen.insert(plat::pathKey(ji.archive)).second) pending.push_back(ji);
        for (auto& a : arcs) { JobInfo ji; if (!seen.count(plat::pathKey(a)) && loadJob(a, ji)) { seen.insert(plat::pathKey(a)); pending.push_back(ji); } }
        for (auto& ji : pending) if (ji.opt.inputs.size() == 1) pendingByInput[plat::pathKey(ji.opt.inputs[0])] = ji;
    }
    // Name each archive after its original; if two originals would get the same name, keep their full names.
    std::map<std::string, int> uses; std::set<std::string> srcKeys;
    for (auto& c : cands) { uses[plat::pathKey(plat::join(c.destDir, c.stem + ext))]++; srcKeys.insert(plat::pathKey(c.src)); }
    std::set<std::string> taken; uint64_t exists = 0, clash = 0;
    for (auto& c : cands) {
        auto pit = pendingByInput.find(plat::pathKey(c.src));
        if (pit != pendingByInput.end()) {
            plat::Stat ast;
            if (plat::stat(pit->second.archive, ast)) {
                BatchJob j; j.opt = base; j.opt.archive = pit->second.archive; j.opt.inputs = pit->second.opt.inputs; j.opt.finalizeOnly = false; j.bytes = c.bytes;
                taken.insert(plat::pathKey(pit->second.archive));
                plan.totalBytes += c.bytes; plan.largestJob = std::max(plan.largestJob, c.bytes);
                plan.jobs.push_back(std::move(j));
                continue;
            }
            discardStaleJournal(pit->second.archive);   // the crash came before that archive was even created
        }
        std::string arc = plat::join(c.destDir, c.stem + ext), key = plat::pathKey(arc);
        if (uses[key] > 1 || srcKeys.count(key)) { arc = plat::join(c.destDir, c.full + ext); key = plat::pathKey(arc); }
        if (!taken.insert(key).second || srcKeys.count(key)) { clash++; continue; }
        plat::Stat st; JobInfo ji;
        if (plat::stat(arc, st) && !loadJob(arc, ji)) { exists++; continue; }   // finished earlier: leave it and its original alone
        BatchJob j; j.opt = base; j.opt.inputs.assign(1, c.src); j.opt.archive = arc; j.opt.finalizeOnly = false; j.bytes = c.bytes;
        plan.totalBytes += c.bytes; plan.largestJob = std::max(plan.largestJob, c.bytes);
        plan.jobs.push_back(std::move(j));
    }
    // Jobs from an earlier run that were interrupted after their original was already consumed: finish them too.
    {
        std::vector<std::string> roots;
        for (auto& in0 : base.inputs) roots.push_back(plat::pathKey(in0));
        for (auto& ji : pending) {
            plat::Stat ast;
            if (ji.opt.inputs.size() != 1 || taken.count(plat::pathKey(ji.archive)) || !plat::stat(ji.archive, ast)) continue;
            std::string k = plat::pathKey(ji.opt.inputs[0]); bool under = false;
            for (auto& r : roots) if (k == r || (k.size() > r.size() && k.compare(0, r.size(), r) == 0 && (k[r.size()] == '/' || k[r.size()] == '\\'))) under = true;
            if (!under) continue;
            BatchJob j; j.opt = base; j.opt.archive = ji.archive; j.opt.inputs = ji.opt.inputs; j.opt.finalizeOnly = false; j.bytes = 0;
            taken.insert(plat::pathKey(ji.archive));
            plan.jobs.push_back(std::move(j));
        }
    }
    if (exists) plan.notes.push_back(std::to_string(exists) + " item(s) will be skipped because an archive with that name already exists.");
    if (clash) plan.notes.push_back(std::to_string(clash) + " item(s) will be skipped because their archive names would collide.");
    if (plan.jobs.empty() && plan.error.empty()) plan.error = "Nothing to do: there are no items left to compress.";
    return plan;
}

BatchResult runBatch(const std::vector<BatchJob>& jobs, Callbacks& cb) {
    BatchResult br; br.total = jobs.size();
    Wrap w; w.cb = &cb; w.count = jobs.size();
    for (auto& j : jobs) w.total += j.bytes;
    auto warn = [&](const std::string& s) { if (br.r.warnings.size() < 500) br.r.warnings.push_back(s); };
    bool stopped = false;
    for (size_t i = 0; i < jobs.size() && !stopped; i++) {
        int m = cb.poll();
        if (m == 1) { br.r.paused = true; br.r.resumable = true; break; }
        if (m == 2) { br.r.partial = true; break; }
        w.idx = i + 1; w.last = 0;
        plat::makeDirs(plat::parentDir(plat::absPath(jobs[i].opt.archive)));
        Result r = run(jobs[i].opt, w);
        std::string label = plat::baseName(jobs[i].opt.inputs.empty() ? jobs[i].opt.archive : jobs[i].opt.inputs[0]);
        for (auto& x : r.warnings) warn(x);
        w.acc.packedBytes += r.final.packedBytes; w.acc.reclaimedBytes += r.final.reclaimedBytes; w.acc.filesDone += r.final.filesDone;
        if (r.ok && !r.partial) { br.done++; w.acc.doneBytes += jobs[i].bytes; continue; }
        w.acc.doneBytes += std::min(jobs[i].bytes, r.final.doneBytes);
        if (r.ok && r.partial) { br.done++; br.r.partial = true; stopped = true; }
        else if (r.paused) { br.r.paused = true; br.r.resumable = true; stopped = true; }
        else if (r.resumable) { br.r.error = label + ": " + r.error; br.r.resumable = true; stopped = true; }
        else { br.skipped++; warn("Skipped " + label + ": " + r.error); }
    }
    br.r.ok = br.r.error.empty() && !br.r.paused;
    if (br.r.ok && br.done + br.skipped < br.total) br.r.partial = true;
    br.r.final = w.acc; br.r.final.totalBytes = w.total; br.r.final.jobIndex = br.done; br.r.final.jobCount = br.total;
    cb.progress(br.r.final);
    return br;
}

} // namespace nz
