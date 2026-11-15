// NibbleZip engine: compress into 7z/ZIP while releasing the source files as it goes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nz {

enum Format { F7Z = 0, FZIP = 1 };

struct Options {
    std::vector<std::string> inputs;
    std::string archive;
    int format = F7Z;
    int level = 1;            // 0 fast, 1 normal, 2 maximum, 3 ultra
    bool eat = true;          // release sources as they are verified in the archive
    bool finalizeOnly = false; // resume: just close the archive with what is already inside
    uint64_t testStep = 0;    // testing: force a tiny step size
};

struct Progress {
    uint64_t totalBytes = 0, doneBytes = 0, packedBytes = 0, reclaimedBytes = 0;
    uint64_t filesTotal = 0, filesDone = 0;
    uint64_t jobIndex = 0, jobCount = 0;   // when several archives are being made in one go
    std::string current;
};

struct Callbacks {
    virtual ~Callbacks() {}
    virtual void progress(const Progress&) {}
    // 0 = keep going, 1 = stop now (job stays resumable), 2 = finish after the current file
    virtual int poll() { return 0; }
};

struct Preflight {
    bool ok = false;
    std::string error;
    std::vector<std::string> notes;
    uint64_t totalBytes = 0, files = 0, dirs = 0, freeBytes = 0, needBytes = 0, stepBytes = 0, largest = 0;
    bool sameVolume = true, holes = true;
};

struct Result {
    bool ok = false;        // archive complete and closed
    bool paused = false;    // stopped on request; resumable
    bool resumable = false; // an error happened but the job can be resumed
    bool partial = false;   // closed early on request; remaining sources untouched
    std::string error;
    std::vector<std::string> warnings;
    Progress final;
};

struct JobInfo {
    std::string archive;
    Options opt;
    bool midFile = false;
    std::string midFileName;
    uint64_t filesDone = 0, packedBytes = 0;
};

struct VerifyResult {
    bool ok = false, cancelled = false;
    std::string error;
    uint64_t files = 0, bytes = 0;
};

// ---- several archives in one go (one per item, or one per file)
enum OutputMode { OutSingle = 0, OutPerItem = 1, OutPerFile = 2 };
struct BatchJob { Options opt; uint64_t bytes = 0; };
struct BatchPlan {
    std::vector<BatchJob> jobs;
    std::vector<std::string> notes;
    std::string error;
    uint64_t totalBytes = 0, largestJob = 0;
};
struct BatchResult { Result r; uint64_t done = 0, total = 0, skipped = 0; };
// outDir empty = put each archive next to its original.
BatchPlan planBatch(const Options& base, int mode, const std::string& outDir);
BatchResult runBatch(const std::vector<BatchJob>& jobs, Callbacks& cb);
std::string deletionLogPath();
void discardStaleJournal(const std::string& archive);   // only acts when the archive itself does not exist

Preflight preflight(const Options& opt);
// Reads a finished 7z or ZIP archive from disk, decompresses every file and checks it against its stored checksum.
VerifyResult verifyArchive(const std::string& archive, Callbacks& cb);
Result run(const Options& opt, Callbacks& cb);
bool loadJob(const std::string& archive, JobInfo& info);
std::vector<JobInfo> pendingJobs();
std::string journalPathFor(const std::string& archive);
std::string humanSize(uint64_t n);

} // namespace nz
