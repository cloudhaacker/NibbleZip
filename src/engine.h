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
    uint64_t split = 0;       // 0 = one file; otherwise write parts of this many bytes (name.7z.001, .002, ...)
    bool finalizeOnly = false; // resume: just close the archive with what is already inside
    uint64_t testStep = 0;    // testing: force a tiny step size
};

struct Progress {
    uint64_t totalBytes = 0, doneBytes = 0, packedBytes = 0, reclaimedBytes = 0;
    uint64_t filesTotal = 0, filesDone = 0;
    uint64_t jobIndex = 0, jobCount = 0;   // when several archives are being made in one go
    int phase = 0;                         // extraction: 1 = checking the archive first, 2 = extracting
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
    uint64_t estBytes = 0;   // rough estimate of the finished archive size, from samples (0 = not estimated)
    bool sameVolume = true, holes = true;
};

struct Result {
    bool ok = false;        // archive complete and closed
    bool paused = false;    // stopped on request; resumable
    bool resumable = false; // an error happened but the job can be resumed
    bool partial = false;   // closed early on request; remaining sources untouched
    bool archiveRemoved = false;   // undo only: the archive held nothing else, so it was removed
    std::string error;
    std::vector<std::string> warnings;
    Progress final;
};

struct JobInfo {
    std::string archive;
    Options opt;
    bool midFile = false;
    std::string midFileName, midFileSrc;
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

// ---- extraction
struct ExtractOptions {
    std::string archive;      // a .7z or .zip, or the .001 part of a split one
    std::string dest;         // empty = a new folder next to the archive, named after it
    bool eat = true;          // shrink the archive as its files come out, and remove it at the end
    bool precheck = true;     // decompress and check the whole archive before changing anything
    uint64_t testInterval = 0; // testing: commit this often
};
struct ExtractResult {
    bool ok = false, paused = false, resumable = false, archiveRemoved = false;
    std::string error, dest;
    std::vector<std::string> warnings;
    uint64_t files = 0, bytes = 0;
    Progress final;
};
struct ExtractInfo { std::string archive, dest; bool eat = true; };
// Extracts a 7z or ZIP. With eat=true the archive gives its disk space back as it is consumed.
// An interrupted extraction is continued by calling this again with the same archive.
ExtractResult extractArchive(const ExtractOptions& opt, Callbacks& cb);
std::vector<ExtractInfo> pendingExtractions();
bool loadExtraction(const std::string& archive, ExtractInfo& info);
// Drops an unfinished "extract only" job (the archive was never changed). Refuses for space-reclaiming jobs.
bool forgetExtraction(const std::string& archive);
// A new folder for an archive's files: next to the archive, or inside `parent` when given. Never an existing name.
std::string defaultExtractDest(const std::string& archive, const std::string& parent = "");
// Every unfinished job (stopped or not) claims what it works on: a compression job its inputs, archive and
// journal; an extraction its archive and destination folder. Returns a message if any of `paths` overlaps
// such a claim (the same path, inside it, or containing it), or "" if they are free. `ownArchive` is ignored.
std::string claimConflict(const std::vector<std::string>& paths, const std::string& ownArchive);

Preflight preflight(const Options& opt);
// Reads a finished 7z or ZIP archive from disk, decompresses every file and checks it against its stored checksum.
VerifyResult verifyArchive(const std::string& archive, Callbacks& cb);
Result run(const Options& opt, Callbacks& cb);
// For a job stopped part-way through a large file: puts that file back exactly as it was, then closes the archive
// with whatever else it already holds (or removes it if it holds nothing).
Result undoPartial(const std::string& archive, Callbacks& cb);
// Rough estimate of the archive size for these inputs, by compressing samples. Returns 0 if it cannot tell.
uint64_t estimateArchiveSize(const std::vector<std::string>& inputs, int format, int level, uint64_t& totalBytes);
bool loadJob(const std::string& archive, JobInfo& info);
std::vector<JobInfo> pendingJobs();
std::string journalPathFor(const std::string& archive);
std::string humanSize(uint64_t n);
bool archiveExists(const std::string& archive);   // as a single file or as split parts

} // namespace nz
