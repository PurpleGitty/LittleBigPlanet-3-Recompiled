// libkernel files and directories, through core::vfs, plus libSceDiscMap.
//
// fds 0-2 are the console streams; game files start at 3. Every open is
// logged once per path as "[fs] ...", which shows what the game loads.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/runner.h"
#include "core/vfs.h"
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::kernel {
namespace {

namespace fs = std::filesystem;

enum OpenFlags : int {
    kOWronly = 0x1, kORdwr = 0x2, kOAccMode = 0x3, kOAppend = 0x8,
    kOCreat = 0x200, kOTrunc = 0x400, kOExcl = 0x800, kODirectory = 0x20000,
};
enum Whence : int { kSeekSet = 0, kSeekCur = 1, kSeekEnd = 2 };
constexpr uint16_t kSIfDir = 0040000, kSIfReg = 0100000;
constexpr int kEROFS = 30;
constexpr int kFirstFd = 3;
constexpr uint8_t kDtDir = 4, kDtReg = 8;

struct Timespec { int64_t sec, nsec; };

struct Stat {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    Timespec atim, mtim, ctim;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    Timespec birthtim;
};
static_assert(sizeof(Stat) == 0x78, "OrbisKernelStat layout mismatch");

struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool isDir = false;
    bool append = false;
    std::string guest;
    fs::path host;
    std::vector<uint8_t> dirents;  // FreeBSD-style records for getdirentries
    int64_t dirPos = 0;
    ~File() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

std::mutex g_mutex;
std::unordered_map<int, std::shared_ptr<File>> g_files;
int g_nextFd = kFirstFd;
std::set<std::string> g_logged;

std::string Utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

int FromWin32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return kENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return kEACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return kEEXIST;
    case ERROR_DIR_NOT_EMPTY: return kENOTEMPTY;
    case ERROR_DISK_FULL: return kENOSPC;
    default: return kEIO;
    }
}

int FromStd(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return kENOENT;
    if (ec == std::errc::file_exists) return kEEXIST;
    if (ec == std::errc::directory_not_empty) return kENOTEMPTY;
    if (ec == std::errc::permission_denied) return kEACCES;
    return kEIO;
}

void LogOnce(const char* what, const std::string& guest, const std::string& detail) {
    std::lock_guard lk(g_mutex);
    if (!g_logged.insert(std::string(what) + guest).second) return;
    std::fprintf(stderr, "[fs] %s %s%s\n", what, guest.c_str(), detail.c_str());
}

std::shared_ptr<File> Get(int fd) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(fd);
    return it == g_files.end() ? nullptr : it->second;
}

Timespec FromFileTime(const FILETIME& ft) {
    int64_t t = (static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    return {t / 10'000'000, t % 10'000'000 * 100};
}

void FillStat(const WIN32_FILE_ATTRIBUTE_DATA& a, Stat* st) {
    std::memset(st, 0, sizeof(*st));
    const bool dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    st->mode = dir ? (kSIfDir | 0777) : (kSIfReg | 0666);
    st->nlink = 1;
    st->size = dir ? 0 : (static_cast<int64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow);
    st->blksize = 16 * 1024;
    st->blocks = (st->size + 511) / 512;
    st->atim = FromFileTime(a.ftLastAccessTime);
    st->mtim = FromFileTime(a.ftLastWriteTime);
    st->ctim = st->mtim;
    st->birthtim = FromFileTime(a.ftCreationTime);
}

int StatHost(const fs::path& p, Stat* st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return SceErr(FromWin32(GetLastError()));
    FillStat(a, st);
    return SCE_OK;
}

void AddDirent(std::vector<uint8_t>& out, uint32_t ino, uint8_t type, const std::string& name) {
    const size_t namlen = std::min<size_t>(name.size(), 255);
    const uint16_t reclen = static_cast<uint16_t>((8 + namlen + 1 + 7) & ~size_t{7});
    const size_t at = out.size();
    out.resize(at + reclen, 0);
    std::memcpy(&out[at], &ino, 4);
    std::memcpy(&out[at + 4], &reclen, 2);
    out[at + 6] = type;
    out[at + 7] = static_cast<uint8_t>(namlen);
    std::memcpy(&out[at + 8], name.data(), namlen);
}

// Union of all layers' entries, highest-priority layer wins on name clashes.
std::vector<uint8_t> BuildDirents(const core::vfs::Resolved& r) {
    std::vector<uint8_t> out;
    uint32_t ino = 1;
    AddDirent(out, ino++, kDtDir, ".");
    AddDirent(out, ino++, kDtDir, "..");
    std::set<std::string> seen;
    std::error_code ec;
    for (const auto& dir : core::vfs::DirectoryLayers(r)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::string name = Utf8(e.path().filename());
            if (!seen.insert(name).second) continue;
            AddDirent(out, ino++, e.is_directory(ec) ? kDtDir : kDtReg, name);
        }
    }
    return out;
}

int OpenImpl(const char* path, int flags) {
    if (!path) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (r.status == core::vfs::Status::BadPath) {
        LogOnce("open (unknown mount)", path, "");
        return SceErr(kENOENT);
    }
    const bool wantsWrite = (flags & kOAccMode) != 0 || (flags & (kOCreat | kOTrunc));
    if (wantsWrite && !r.writable) return SceErr(kEROFS);

    auto f = std::make_shared<File>();
    f->guest = r.guest;
    f->host = r.host;
    std::error_code ec;

    if (r.status == core::vfs::Status::Ok && fs::is_directory(r.host, ec)) {
        if (flags & kOAccMode) return SceErr(kEISDIR);
        f->isDir = true;
        f->dirents = BuildDirents(r);
    } else {
        if (flags & kODirectory) return SceErr(r.status == core::vfs::Status::Ok ? kENOTDIR : kENOENT);
        if (r.status == core::vfs::Status::NotFound && !(flags & kOCreat)) {
            LogOnce("open (missing)", r.guest, "");
            return SceErr(kENOENT);
        }
        DWORD access = GENERIC_READ;
        if ((flags & kOAccMode) == kOWronly) access = GENERIC_WRITE;
        if ((flags & kOAccMode) == kORdwr) access = GENERIC_READ | GENERIC_WRITE;
        DWORD disposition = OPEN_EXISTING;
        if ((flags & kOCreat) && (flags & kOExcl)) disposition = CREATE_NEW;
        else if ((flags & kOCreat) && (flags & kOTrunc)) disposition = CREATE_ALWAYS;
        else if (flags & kOCreat) disposition = OPEN_ALWAYS;
        else if (flags & kOTrunc) disposition = TRUNCATE_EXISTING;
        f->handle = CreateFileW(r.host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f->handle == INVALID_HANDLE_VALUE) return SceErr(FromWin32(GetLastError()));
        f->append = flags & kOAppend;
    }

    LogOnce("open", r.guest, "  ->  " + Utf8(r.host));
    std::lock_guard lk(g_mutex);
    if (g_nextFd >= 64 && g_nextFd <= 1023) g_nextFd = 1024;  // 64..1023 are socket numbers (net_offline.cpp)
    const int fd = g_nextFd++;
    g_files[fd] = std::move(f);
    return fd;
}

int64_t ReadImpl(int fd, void* buf, uint64_t len, const int64_t* offset) {
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (f->isDir) return SceErr(kEISDIR);
    uint64_t total = 0;
    auto* out = static_cast<uint8_t*>(buf);
    // Read through a bounce buffer: the destination may be write-protected by
    // the GPU's write tracking, and ReadFile fails on such pages rather than
    // faulting. The memcpy faults normally and the tracker unprotects.
    thread_local std::vector<uint8_t> bounce(1 << 20);
    while (total < len) {
        DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(len - total, bounce.size()));
        DWORD got = 0;
        OVERLAPPED ov{};
        OVERLAPPED* pov = nullptr;
        if (offset) {
            const uint64_t at = static_cast<uint64_t>(*offset) + total;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            pov = &ov;
        }
        if (!ReadFile(f->handle, bounce.data(), chunk, &got, pov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            return total ? static_cast<int64_t>(total) : SceErr(FromWin32(e));
        }
        std::memcpy(out + total, bounce.data(), got);
        total += got;
        if (got < chunk) break;
    }
    // LBP3_TRACE_READS=N logs the first N archive (.farc) reads with thread and offset.
    {
        static const int traceLimit = [] {
            const char* v = std::getenv("LBP3_TRACE_READS");
            return v ? std::atoi(v) : 0;
        }();
        static std::atomic<int> traced{0};
        const auto endsWith = [&](const char* ext) {
            const size_t n = std::strlen(ext);
            return f->guest.size() > n && f->guest.compare(f->guest.size() - n, n, ext) == 0;
        };
        if (traceLimit && (endsWith(".farc") || endsWith(".fsb") || endsWith("_orbis")) &&
            traced++ < traceLimit) {
            std::fprintf(stderr, "[fs] %s: read %llu bytes at %lld -> %llu [%s]\n", f->guest.c_str(),
                         static_cast<unsigned long long>(len), offset ? static_cast<long long>(*offset) : -1LL,
                         static_cast<unsigned long long>(total), core::ThreadName(GetCurrentThreadId()).c_str());
        }
    }
    // LBP3_TRACE_FILE=text: every read of files whose path contains text, with the file
    // position after it and the size (does a stream reach its end, and wrap around?).
    if (static const char* trace = std::getenv("LBP3_TRACE_FILE"); trace && f->guest.find(trace) != std::string::npos) {
        LARGE_INTEGER zero{}, pos{}, size{};
        SetFilePointerEx(f->handle, zero, &pos, FILE_CURRENT);
        GetFileSizeEx(f->handle, &size);
        std::fprintf(stderr, "[fs] trace %s: read %llu at %lld -> %llu (pos now %lld of %lld)\n", f->guest.c_str(),
                     static_cast<unsigned long long>(len), offset ? static_cast<long long>(*offset) : -1LL,
                     static_cast<unsigned long long>(total), static_cast<long long>(pos.QuadPart),
                     static_cast<long long>(size.QuadPart));
    }
    // Read activity, once every 5 s while there is some: tells "loading slowly"
    // apart from "not loading at all".
    {
        static std::atomic<uint64_t> bytes{0}, reads{0};
        static std::atomic<int64_t> lastReport{0};
        bytes += total;
        ++reads;
        const int64_t now = static_cast<int64_t>(GetTickCount64() / 5000);
        int64_t prev = lastReport.load();
        if (now != prev && lastReport.compare_exchange_strong(prev, now))
            std::fprintf(stderr, "[fs] read %llu KB in %llu reads (last: %s)\n",
                         static_cast<unsigned long long>(bytes.exchange(0) / 1024),
                         static_cast<unsigned long long>(reads.exchange(0)), f->guest.c_str());
    }
    return static_cast<int64_t>(total);
}

int64_t WriteImpl(int fd, const void* buf, uint64_t len, const int64_t* offset) {
    if (fd == 1 || fd == 2) {
        // The game's own debug output (printf goes through here). Shown with a
        // prefix so it stands apart from runtime messages.
        std::fprintf(stderr, "[game] %.*s", static_cast<int>(len), static_cast<const char*>(buf));
        if (len && static_cast<const char*>(buf)[len - 1] != '\n') std::fputc('\n', stderr);
        std::fflush(stderr);
        return static_cast<int64_t>(len);
    }
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (f->isDir) return SceErr(kEISDIR);
    OVERLAPPED ov{};
    OVERLAPPED* pov = nullptr;
    if (offset) {
        ov.Offset = static_cast<DWORD>(*offset);
        ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(*offset) >> 32);
        pov = &ov;
    } else if (f->append) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(f->handle, zero, nullptr, FILE_END);
    }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f->handle, buf, static_cast<DWORD>(std::min<uint64_t>(len, 0xFFFFFFFF)), &wrote, pov);
    {
        static const int traceLimit = [] {
            const char* v = std::getenv("LBP3_TRACE_READS");
            return v ? std::atoi(v) : 0;
        }();
        static std::atomic<int> traced{0};
        if (traceLimit && traced++ < traceLimit)
            std::fprintf(stderr, "[fs] %s: write %llu bytes at %lld -> %s %lu [%s]\n", f->guest.c_str(),
                         static_cast<unsigned long long>(len), offset ? static_cast<long long>(*offset) : -1LL,
                         ok ? "ok" : "FAILED", ok ? wrote : GetLastError(), core::ThreadName(GetCurrentThreadId()).c_str());
    }
    if (!ok) return SceErr(FromWin32(GetLastError()));
    return wrote;
}

// sce* return values are either a count or an SCE error; POSIX wants -1 + errno.
int64_t PosixCount(int64_t r) {
    if (r >= 0) return r;
    *__error() = ToErrno(static_cast<int>(r));
    return -1;
}

}  // namespace

int PS4_SYSV_ABI sceKernelOpen(const char* path, int flags, uint16_t mode) {
    (void)mode;
    return OpenImpl(path, flags);
}

int PS4_SYSV_ABI sceKernelClose(int fd) {
    std::lock_guard lk(g_mutex);
    // Streamed audio is closed when it has played out: shows whether a voice line ended.
    if (auto it = g_files.find(fd); it != g_files.end() && it->second &&
                                    it->second->guest.find("/audio/") != std::string::npos)
        std::fprintf(stderr, "[fs] close %s\n", it->second->guest.c_str());
    return g_files.erase(fd) ? SCE_OK : SceErr(kEBADF);
}

int64_t PS4_SYSV_ABI sceKernelRead(int fd, void* buf, uint64_t len) { return ReadImpl(fd, buf, len, nullptr); }
int64_t PS4_SYSV_ABI sceKernelPread(int fd, void* buf, uint64_t len, int64_t off) { return ReadImpl(fd, buf, len, &off); }
int64_t PS4_SYSV_ABI sceKernelWrite(int fd, const void* buf, uint64_t len) { return WriteImpl(fd, buf, len, nullptr); }
int64_t PS4_SYSV_ABI sceKernelPwrite(int fd, const void* buf, uint64_t len, int64_t off) {
    return WriteImpl(fd, buf, len, &off);
}

int64_t PS4_SYSV_ABI _read(int fd, void* buf, uint64_t len) { return PosixCount(sceKernelRead(fd, buf, len)); }
int64_t PS4_SYSV_ABI _write(int fd, const void* buf, uint64_t len) { return PosixCount(sceKernelWrite(fd, buf, len)); }

int64_t PS4_SYSV_ABI sceKernelLseek(int fd, int64_t offset, int whence) {
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (f->isDir) {
        int64_t base = whence == kSeekCur ? f->dirPos : whence == kSeekEnd ? static_cast<int64_t>(f->dirents.size()) : 0;
        if (base + offset < 0) return SceErr(kEINVAL);
        f->dirPos = base + offset;
        return f->dirPos;
    }
    static const DWORD kMethods[] = {FILE_BEGIN, FILE_CURRENT, FILE_END};
    if (whence < kSeekSet || whence > kSeekEnd) return SceErr(kEINVAL);
    LARGE_INTEGER dist, now;
    dist.QuadPart = offset;
    if (!SetFilePointerEx(f->handle, dist, &now, kMethods[whence])) return SceErr(kEINVAL);
    return now.QuadPart;
}

int PS4_SYSV_ABI sceKernelFstat(int fd, Stat* st) {
    if (!st) return SceErr(kEFAULT);
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (f->isDir) return StatHost(f->host, st);
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(f->handle, &info)) return SceErr(FromWin32(GetLastError()));
    WIN32_FILE_ATTRIBUTE_DATA a{info.dwFileAttributes, info.ftCreationTime, info.ftLastAccessTime,
                                info.ftLastWriteTime, info.nFileSizeHigh, info.nFileSizeLow};
    FillStat(a, st);
    return SCE_OK;
}

int PS4_SYSV_ABI fstat(int fd, Stat* st) { return PosixResult(sceKernelFstat(fd, st)); }

int PS4_SYSV_ABI sceKernelStat(const char* path, Stat* st) {
    if (!path || !st) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (r.status != core::vfs::Status::Ok) {
        LogOnce("stat (missing)", r.guest.empty() ? path : r.guest, "");
        return SceErr(kENOENT);
    }
    return StatHost(r.host, st);
}

int PS4_SYSV_ABI sceKernelFsync(int fd) {
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (!f->isDir) FlushFileBuffers(f->handle);
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelFtruncate(int fd, int64_t length) {
    auto f = Get(fd);
    if (!f || f->isDir) return SceErr(kEBADF);
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = length;
    if (!SetFileInformationByHandle(f->handle, FileEndOfFileInfo, &eof, sizeof(eof)))
        return SceErr(FromWin32(GetLastError()));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelTruncate(const char* path, int64_t length) {
    if (!path) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (!r.writable) return SceErr(kEROFS);
    std::error_code ec;
    fs::resize_file(r.host, static_cast<uintmax_t>(length), ec);
    return ec ? SceErr(FromStd(ec)) : SCE_OK;
}

int PS4_SYSV_ABI sceKernelMkdir(const char* path, uint16_t mode) {
    (void)mode;
    if (!path) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (r.status == core::vfs::Status::BadPath) return SceErr(kENOENT);
    if (!r.writable) return SceErr(r.status == core::vfs::Status::Ok ? kEEXIST : kEROFS);
    std::error_code ec;
    if (fs::exists(r.host, ec)) return SceErr(kEEXIST);
    if (!fs::create_directory(r.host, ec)) return SceErr(ec ? FromStd(ec) : kEIO);
    LogOnce("mkdir", r.guest, "");
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelRmdir(const char* path) {
    if (!path) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (!r.writable) return SceErr(kEROFS);
    if (!RemoveDirectoryW(r.host.c_str())) return SceErr(FromWin32(GetLastError()));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelUnlink(const char* path) {
    if (!path) return SceErr(kEFAULT);
    core::vfs::Resolved r = core::vfs::Resolve(path);
    if (!r.writable) return SceErr(kEROFS);
    if (!DeleteFileW(r.host.c_str())) return SceErr(FromWin32(GetLastError()));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelRename(const char* from, const char* to) {
    if (!from || !to) return SceErr(kEFAULT);
    core::vfs::Resolved a = core::vfs::Resolve(from), b = core::vfs::Resolve(to);
    if (!a.writable || !b.writable) return SceErr(kEROFS);
    if (!MoveFileExW(a.host.c_str(), b.host.c_str(), MOVEFILE_REPLACE_EXISTING))
        return SceErr(FromWin32(GetLastError()));
    return SCE_OK;
}

int64_t PS4_SYSV_ABI sceKernelGetdirentries(int fd, char* buf, uint64_t nbytes, int64_t* basep) {
    auto f = Get(fd);
    if (!f) return SceErr(kEBADF);
    if (!f->isDir) return SceErr(kENOTDIR);
    if (basep) *basep = f->dirPos;
    // Copy whole records only.
    uint64_t written = 0;
    size_t pos = static_cast<size_t>(f->dirPos);
    while (pos + 8 <= f->dirents.size()) {
        uint16_t reclen;
        std::memcpy(&reclen, &f->dirents[pos + 4], 2);
        if (written + reclen > nbytes) break;
        std::memcpy(buf + written, &f->dirents[pos], reclen);
        written += reclen;
        pos += reclen;
    }
    f->dirPos = static_cast<int64_t>(pos);
    return static_cast<int64_t>(written);
}

// --- libSceDiscMap: everything is installed on the "HDD", there's no disc. -------

constexpr int kDiscMapNoBitmapInfo = static_cast<int>(0x81100004);

int PS4_SYSV_ABI sceDiscMapIsRequestOnHDD(const char*, int64_t, int64_t, int*) { return kDiscMapNoBitmapInfo; }
int PS4_SYSV_ABI DiscMapUnknown8A828CAE(const char*, int64_t, int64_t, int*, int*, int*) { return kDiscMapNoBitmapInfo; }

}  // namespace hle::kernel

namespace hle {

void RegisterKernelFs(core::Linker& linker) {
    using namespace kernel;
    const char* lib = "libkernel";
    HLE_REG(linker, lib, sceKernelOpen);
    HLE_REG(linker, lib, sceKernelClose);
    HLE_REG(linker, lib, sceKernelRead);
    HLE_REG(linker, lib, sceKernelPread);
    HLE_REG(linker, lib, sceKernelWrite);
    HLE_REG(linker, lib, sceKernelPwrite);
    HLE_REG(linker, lib, _read);
    HLE_REG(linker, lib, _write);
    HLE_REG(linker, lib, sceKernelLseek);
    HLE_REG(linker, lib, sceKernelFstat);
    HLE_REG(linker, lib, fstat);
    HLE_REG(linker, "libScePosix", fstat);
    HLE_REG(linker, lib, sceKernelStat);
    HLE_REG(linker, lib, sceKernelFsync);
    HLE_REG(linker, lib, sceKernelFtruncate);
    HLE_REG(linker, lib, sceKernelTruncate);
    HLE_REG(linker, lib, sceKernelMkdir);
    HLE_REG(linker, lib, sceKernelRmdir);
    HLE_REG(linker, lib, sceKernelUnlink);
    HLE_REG(linker, lib, sceKernelRename);
    HLE_REG(linker, lib, sceKernelGetdirentries);
    HLE_REG(linker, "libSceDiscMap", sceDiscMapIsRequestOnHDD);
    // Unnamed export; shadPS4 calls it Func_8A828CAEE7EDD5E9.
    linker.RegisterHleNid("libSceDiscMap", "ioKMruft1ek", reinterpret_cast<void*>(&DiscMapUnknown8A828CAE));
}

}  // namespace hle
