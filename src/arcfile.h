// An archive on disk: either one file, or a series of fixed-size parts (name.7z.001, .002, ...).
#pragma once
#include "platform.h"
#include <memory>
#include <string>
#include <vector>

namespace nz {

class ArcFile {
public:
    ~ArcFile() { close(); }
    bool create(const std::string& base, uint64_t partSize);                      // fails if the archive already exists
    bool openExisting(const std::string& base, uint64_t partSize, bool readOnly); // partSize 0 = single file
    // For extraction: the archive may already be partly released (leading parts deleted, ranges hollowed out).
    bool openForExtract(const std::string& base, uint64_t partSize, uint64_t totalSize, bool writable);
    // Gives back the disk space of everything before `pos`, except the first `keepHead` bytes.
    // Returns false if this drive cannot release space inside a file (the data is then simply left in place).
    bool releaseBefore(uint64_t pos, uint64_t keepHead);
    static bool totalSizeOf(const std::string& base, uint64_t partSize, uint64_t& total);
    void close();
    bool isOpen() const { return open_; }
    bool readAt(uint64_t off, void* buf, size_t n, size_t& got);
    bool writeAt(uint64_t off, const void* buf, size_t n);
    bool flush();
    bool truncate(uint64_t size);
    bool size(uint64_t& sz) { sz = total_; return open_; }

    static std::string partName(const std::string& base, uint64_t index);   // index 0 -> ".001"
    // Deletes the archive file or all its parts. Give totalSize when some parts may already be missing.
    static void removeAll(const std::string& base, uint64_t partSize, uint64_t totalSize = 0);
    static bool exists(const std::string& base);                             // as one file or as parts
    // If `path` is the first part of a split archive, returns its base name and part size.
    static void detect(const std::string& path, std::string& base, uint64_t& partSize);
    static bool isPartOf(const std::string& pathKey, const std::string& baseKey);

private:
    struct Slot { uint64_t idx = 0; plat::File f; bool dirty = false; };
    plat::File* part(uint64_t idx, bool create);
    std::string base_;
    uint64_t part_ = 0, total_ = 0;
    bool open_ = false, ro_ = false;
    uint64_t released_ = 0;
    std::vector<std::unique_ptr<Slot>> slots_;
};

} // namespace nz
