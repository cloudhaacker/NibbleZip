// NibbleZip platform layer: thin wrappers over Win32 / POSIX. All paths are UTF-8.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace plat {

struct Stat {
    bool exists = false, isDir = false, isLink = false;
    uint64_t size = 0;
    uint64_t mtime = 0;   // Windows FILETIME units (100ns since 1601-01-01 UTC)
    uint32_t attr = 0;    // Windows-style attributes
};
struct DirEnt { std::string name; Stat st; };

enum OpenMode { OpenRead, OpenReadPunch, OpenRWExisting, OpenRWCreateNew, OpenRWAlways };

class File {
public:
    File() {}
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool open(const std::string& path, OpenMode m);
    void close();
    bool isOpen() const;
    bool readAt(uint64_t off, void* buf, size_t n, size_t& got);
    bool writeAt(uint64_t off, const void* buf, size_t n);
    bool flush();
    bool truncate(uint64_t size);
    bool size(uint64_t& sz);
    bool punchHole(uint64_t off, uint64_t len);
private:
#ifdef _WIN32
    void* h_ = (void*)(intptr_t)-1;
    bool sparse_ = false;
#else
    int fd_ = -1;
#endif
};

bool stat(const std::string& path, Stat& st);
bool listDir(const std::string& path, std::vector<DirEnt>& out);
bool removeFile(const std::string& path);
bool removeDir(const std::string& path);
bool makeDirs(const std::string& path);
bool freeSpace(const std::string& dir, uint64_t& freeBytes);
std::string volumeId(const std::string& path);
bool supportsHoles(const std::string& path);
uint64_t physMemory();
unsigned cpuCount();
std::string absPath(const std::string& p);
std::string parentDir(const std::string& p);
std::string baseName(const std::string& p);
std::string join(const std::string& a, const std::string& b);
std::string appDataDir();
std::string lastError();
bool samePath(const std::string& a, const std::string& b);
std::string pathKey(const std::string& p);   // normalized for comparisons
extern const char SEP;

} // namespace plat
