#include "platform.h"
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

namespace plat {
const char SEP = '\\';

static std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// Long-path form so paths over 260 chars work.
static std::wstring LP(const std::string& s) {
    std::wstring w = W(s);
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    if (w.size() >= 3 && w[1] == L':' && w[2] == L'\\') return L"\\\\?\\" + w;
    return w;
}
static uint64_t ft64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

std::string lastError() {
    DWORD e = GetLastError();
    wchar_t buf[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, 511, nullptr);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) n--;
    return U(std::wstring(buf, n)) + " (code " + std::to_string(e) + ")";
}

bool File::open(const std::string& path, OpenMode m) {
    close();
    std::wstring w = LP(path);
    DWORD access = GENERIC_READ, share = FILE_SHARE_READ, disp = OPEN_EXISTING, flags = FILE_ATTRIBUTE_NORMAL;
    switch (m) {
        case OpenRead: flags |= FILE_FLAG_SEQUENTIAL_SCAN; break;
        case OpenReadPunch: access |= GENERIC_WRITE; flags |= FILE_FLAG_SEQUENTIAL_SCAN; break;
        case OpenRWExisting: access |= GENERIC_WRITE; break;
        case OpenRWCreateNew: access |= GENERIC_WRITE; disp = CREATE_NEW; break;
        case OpenRWAlways: access |= GENERIC_WRITE; disp = OPEN_ALWAYS; break;
    }
    HANDLE h = CreateFileW(w.c_str(), access, share, nullptr, disp, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE && m == OpenReadPunch && GetLastError() == ERROR_ACCESS_DENIED) {
        DWORD a = GetFileAttributesW(w.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            h = CreateFileW(w.c_str(), access, share, nullptr, disp, flags, nullptr);
        } else SetLastError(ERROR_ACCESS_DENIED);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    h_ = h; sparse_ = false;
    return true;
}
void File::close() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle((HANDLE)h_); h_ = INVALID_HANDLE_VALUE; } }
bool File::isOpen() const { return h_ != INVALID_HANDLE_VALUE; }
bool File::readAt(uint64_t off, void* buf, size_t n, size_t& got) {
    got = 0;
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        DWORD want = (DWORD)std::min<size_t>(n, 64u << 20), rd = 0;
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile((HANDLE)h_, p, want, &rd, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return false;
        }
        if (rd == 0) break;
        p += rd; off += rd; n -= rd; got += rd;
    }
    return true;
}
bool File::writeAt(uint64_t off, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        DWORD want = (DWORD)std::min<size_t>(n, 64u << 20), wr = 0;
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(off >> 32);
        if (!WriteFile((HANDLE)h_, p, want, &wr, &ov)) return false;
        if (wr == 0) { SetLastError(ERROR_DISK_FULL); return false; }
        p += wr; off += wr; n -= wr;
    }
    return true;
}
bool File::flush() { return FlushFileBuffers((HANDLE)h_) != 0; }
bool File::truncate(uint64_t size) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx((HANDLE)h_, li, nullptr, FILE_BEGIN)) return false;
    return SetEndOfFile((HANDLE)h_) != 0;
}
bool File::size(uint64_t& sz) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx((HANDLE)h_, &li)) return false;
    sz = (uint64_t)li.QuadPart; return true;
}
bool File::punchHole(uint64_t off, uint64_t len) {
    DWORD ret = 0;
    if (!sparse_) {
        if (!DeviceIoControl((HANDLE)h_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr)) return false;
        sparse_ = true;
    }
    FILE_ZERO_DATA_INFORMATION z;
    z.FileOffset.QuadPart = (LONGLONG)off;
    z.BeyondFinalZero.QuadPart = (LONGLONG)(off + len);
    return DeviceIoControl((HANDLE)h_, FSCTL_SET_ZERO_DATA, &z, sizeof(z), nullptr, 0, &ret, nullptr) != 0;
}

static void fillStat(Stat& st, DWORD attr, DWORD szHi, DWORD szLo, const FILETIME& mt) {
    st.exists = true;
    st.attr = attr;
    st.isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st.isLink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st.size = st.isDir ? 0 : (((uint64_t)szHi << 32) | szLo);
    st.mtime = ft64(mt);
}
bool stat(const std::string& path, Stat& st) {
    st = Stat();
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(LP(path).c_str(), GetFileExInfoStandard, &d)) return false;
    fillStat(st, d.dwFileAttributes, d.nFileSizeHigh, d.nFileSizeLow, d.ftLastWriteTime);
    return true;
}
bool listDir(const std::string& path, std::vector<DirEnt>& out) {
    out.clear();
    std::wstring pat = LP(path);
    if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
    pat += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        DirEnt e; e.name = U(fd.cFileName);
        fillStat(e.st, fd.dwFileAttributes, fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    DWORD err = GetLastError();
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirEnt& a, const DirEnt& b) { return a.name < b.name; });
    return err == ERROR_NO_MORE_FILES;
}
bool removeFile(const std::string& path) {
    std::wstring w = LP(path);
    if (DeleteFileW(w.c_str())) return true;
    DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(w.c_str())) return true;
        DWORD e = GetLastError();
        SetFileAttributesW(w.c_str(), a);
        SetLastError(e);
    }
    return false;
}
bool setMTime(const std::string& path, uint64_t mtime) {
    HANDLE h = CreateFileW(LP(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft; ft.dwLowDateTime = (DWORD)(mtime & 0xFFFFFFFFu); ft.dwHighDateTime = (DWORD)(mtime >> 32);
    BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
}
bool removeDir(const std::string& path) {
    std::wstring w = LP(path);
    if (RemoveDirectoryW(w.c_str())) return true;
    DWORD e = GetLastError(), a = GetFileAttributesW(w.c_str());
    if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        if (RemoveDirectoryW(w.c_str())) return true;
        SetFileAttributesW(w.c_str(), a);
    }
    return false;
}
bool makeDirs(const std::string& path) {
    Stat st;
    if (stat(path, st)) return st.isDir;
    std::string par = parentDir(path);
    if (!par.empty() && par != path) makeDirs(par);
    return CreateDirectoryW(LP(path).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool freeSpace(const std::string& dir, uint64_t& freeBytes) {
    ULARGE_INTEGER avail, total, fr;
    if (!GetDiskFreeSpaceExW(LP(dir).c_str(), &avail, &total, &fr)) return false;
    freeBytes = avail.QuadPart; return true;
}
static std::wstring volRoot(const std::string& path) {
    wchar_t buf[MAX_PATH + 1] = {0};
    std::wstring w = W(absPath(path));
    if (!GetVolumePathNameW(w.c_str(), buf, MAX_PATH)) {
        if (w.size() >= 3 && w[1] == L':') return w.substr(0, 3);
        return w;
    }
    return buf;
}
std::string volumeId(const std::string& path) {
    std::wstring r = volRoot(path);
    wchar_t name[64] = {0};
    if (GetVolumeNameForVolumeMountPointW(r.c_str(), name, 63)) return U(name);
    std::string s = U(r);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool supportsHoles(const std::string& path) {
    wchar_t ev[8];
    if (GetEnvironmentVariableW(L"NIBBLEZIP_FORCEHOLES", ev, 8) == 1 && ev[0] == L'1') return true;   // test hook
    std::wstring r = volRoot(path);
    DWORD flags = 0;
    if (!GetVolumeInformationW(r.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) return false;
    return (flags & FILE_SUPPORTS_SPARSE_FILES) != 0;
}
uint64_t physMemory() {
    MEMORYSTATUSEX m; m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return 4ull << 30;
    return m.ullTotalPhys;
}
unsigned cpuCount() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? n : 1;
}
std::string absPath(const std::string& p) {
    std::wstring w = W(p);
    for (auto& c : w) if (c == L'/') c = L'\\';
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, 0);
    DWORD m = GetFullPathNameW(w.c_str(), n, &out[0], nullptr);
    out.resize(m);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return U(out);
}
std::string appDataDir() {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"NIBBLEZIP_HOME", buf, 32767);
    if (n > 0 && n < 32767) return absPath(U(std::wstring(buf, n)));
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32767);
    if (n > 0 && n < 32767) return U(std::wstring(buf, n)) + "\\NibbleZip";
    return "C:\\NibbleZip";
}
std::string pathKey(const std::string& p) {
    std::wstring w = W(absPath(p));
    if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
    return U(w);
}
} // namespace plat

#else // ---------------------------------------------------------------- POSIX
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/falloc.h>
#endif

namespace plat {
const char SEP = '/';
std::string lastError() { return std::string(strerror(errno)) + " (errno " + std::to_string(errno) + ")"; }

bool File::open(const std::string& path, OpenMode m) {
    close();
    int flags = O_RDONLY;
    switch (m) {
        case OpenRead: break;
        case OpenReadPunch: flags = O_RDWR; break;
        case OpenRWExisting: flags = O_RDWR; break;
        case OpenRWCreateNew: flags = O_RDWR | O_CREAT | O_EXCL; break;
        case OpenRWAlways: flags = O_RDWR | O_CREAT; break;
    }
    fd_ = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    return fd_ >= 0;
}
void File::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
bool File::isOpen() const { return fd_ >= 0; }
bool File::readAt(uint64_t off, void* buf, size_t n, size_t& got) {
    got = 0; uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        ssize_t r = pread(fd_, p, n, (off_t)off);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) break;
        p += r; off += r; n -= r; got += r;
    }
    return true;
}
bool File::writeAt(uint64_t off, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        ssize_t r = pwrite(fd_, p, n, (off_t)off);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        p += r; off += r; n -= r;
    }
    return true;
}
bool File::flush() { return fsync(fd_) == 0; }
bool File::truncate(uint64_t size) { return ftruncate(fd_, (off_t)size) == 0; }
bool File::size(uint64_t& sz) { struct stat s; if (fstat(fd_, &s)) return false; sz = (uint64_t)s.st_size; return true; }
bool File::punchHole(uint64_t off, uint64_t len) {
#ifdef __linux__
    return fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len) == 0;
#else
    (void)off; (void)len; return false;
#endif
}
static void fill(Stat& st, const struct stat& s) {
    st.exists = true;
    st.isDir = S_ISDIR(s.st_mode);
    st.isLink = S_ISLNK(s.st_mode) || !(S_ISDIR(s.st_mode) || S_ISREG(s.st_mode));
    st.size = st.isDir ? 0 : (uint64_t)s.st_size;
    st.mtime = ((uint64_t)s.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)s.st_mtim.tv_nsec / 100;
    st.attr = st.isDir ? 0x10 : 0x20;
}
bool stat(const std::string& path, Stat& st) {
    st = Stat(); struct stat s;
    if (lstat(path.c_str(), &s)) return false;
    fill(st, s); return true;
}
bool listDir(const std::string& path, std::vector<DirEnt>& out) {
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent* de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        DirEnt e; e.name = de->d_name;
        if (!stat(join(path, e.name), e.st)) continue;
        out.push_back(std::move(e));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEnt& a, const DirEnt& b) { return a.name < b.name; });
    return true;
}
bool removeFile(const std::string& path) { return unlink(path.c_str()) == 0; }
bool setMTime(const std::string& path, uint64_t mtime) {
    if (mtime < 116444736000000000ull) return false;
    uint64_t t = mtime - 116444736000000000ull;
    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = (time_t)(t / 10000000ull); ts[1].tv_nsec = (long)((t % 10000000ull) * 100);
    return utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0;
}
bool removeDir(const std::string& path) { return rmdir(path.c_str()) == 0; }
bool makeDirs(const std::string& path) {
    Stat st;
    if (stat(path, st)) return st.isDir;
    std::string par = parentDir(path);
    if (!par.empty() && par != path) makeDirs(par);
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}
bool freeSpace(const std::string& dir, uint64_t& freeBytes) {
    struct statvfs v;
    if (statvfs(dir.c_str(), &v)) return false;
    freeBytes = (uint64_t)v.f_bavail * v.f_frsize; return true;
}
std::string volumeId(const std::string& path) {
    struct stat s; std::string p = path;
    while (::stat(p.c_str(), &s)) { std::string par = parentDir(p); if (par == p || par.empty()) return "?"; p = par; }
    return std::to_string((unsigned long long)s.st_dev);
}
bool supportsHoles(const std::string&) {
    const char* e = getenv("NIBBLEZIP_NOHOLES");
    return !(e && *e == '1');
}
uint64_t physMemory() { return (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGE_SIZE); }
unsigned cpuCount() { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)n : 1; }
std::string absPath(const std::string& p) {
    std::string s = p;
    if (s.empty() || s[0] != '/') { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) s = std::string(cwd) + "/" + s; }
    // normalize . and ..
    std::vector<std::string> parts; size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('/', i); if (j == std::string::npos) j = s.size();
        std::string c = s.substr(i, j - i); i = j + 1;
        if (c.empty() || c == ".") continue;
        if (c == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(c);
    }
    std::string out;
    for (auto& c : parts) out += "/" + c;
    return out.empty() ? "/" : out;
}
std::string appDataDir() {
    const char* e = getenv("NIBBLEZIP_HOME");
    if (e && *e) return e;
    const char* h = getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.nibblezip";
}
std::string pathKey(const std::string& p) { return absPath(p); }
} // namespace plat
#endif

namespace plat {
std::string parentDir(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    if (i == std::string::npos) return "";
    if (i == 0) return p.substr(0, 1);
    if (i == 2 && p.size() > 1 && p[1] == ':') return p.substr(0, 3);
    return p.substr(0, i);
}
std::string baseName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    if (c == '/' || c == '\\') return a + b;
    return a + SEP + b;
}
bool samePath(const std::string& a, const std::string& b) { return pathKey(a) == pathKey(b); }
}
