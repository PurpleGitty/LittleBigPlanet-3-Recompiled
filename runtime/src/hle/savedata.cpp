// libSceSaveData: every save directory is a normal folder under
// user/savedata/<title id>/<dir name>/, mounted at /savedata0.. while the game
// uses it. Saves can be backed up, copied or swapped with File Explorer.
// Structures and error codes follow shadPS4's save_data library.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "core/vfs.h"
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::savedata {
namespace {

namespace fs = std::filesystem;

constexpr int kErrorParameter = static_cast<int>(0x809F0000);
constexpr int kErrorNotMounted = static_cast<int>(0x809F0004);
constexpr int kErrorExists = static_cast<int>(0x809F0007);
constexpr int kErrorNotFound = static_cast<int>(0x809F0008);
constexpr int kErrorMountFull = static_cast<int>(0x809F000C);
constexpr int kErrorInternal = static_cast<int>(0x809F000B);

enum MountMode : uint32_t { kRdOnly = 1, kRdWr = 2, kCreate = 4, kCreate2 = 32 };
enum MountStatus : uint32_t { kNothing = 0, kCreated = 1 };

constexpr size_t kTitleIdSize = 10;
constexpr size_t kDirNameSize = 32;
constexpr size_t kMountPointSize = 16;
constexpr int kMaxMounts = 16;
constexpr const char* kDefaultTitleId = "CUSA00473";

std::mutex g_mutex;
std::string g_mounted[kMaxMounts];  // mount slot -> dir name ("" = free)

std::string Bounded(const char* s, size_t max) { return s ? std::string(s, strnlen(s, max)) : std::string(); }

fs::path SaveFolder(const char* titleId, const std::string& dirName) {
    std::string title = Bounded(titleId, kTitleIdSize);
    if (title.empty()) title = kDefaultTitleId;
    return core::vfs::UserRoot() / "savedata" / title / fs::u8path(dirName);
}

}  // namespace

struct DirName { char data[kDirNameSize]; };
struct MountPoint { char data[kMountPointSize]; };

struct Mount {
    int32_t userId;
    int32_t pad0;
    const char* titleId;
    const DirName* dirName;
    const void* fingerprint;
    uint64_t blocks;
    uint32_t mountMode;
    uint8_t reserved[32];
};

struct MountResult {
    MountPoint mountPoint;
    uint64_t requiredBlocks;
    uint32_t unused;
    uint32_t mountStatus;
    uint8_t reserved[28];
    int32_t pad0;
};

struct DirNameSearchCond {
    int32_t userId;
    int32_t pad0;
    const char* titleId;
    const DirName* dirName;  // pattern; '%' = any run of characters
    uint32_t key, order;
    uint8_t reserved[32];
};

struct DirNameSearchResult {
    uint32_t hitNum;
    int32_t pad0;
    DirName* dirNames;
    uint32_t dirNamesNum;
    uint32_t setNum;
    void* params;
    void* infos;
    uint8_t reserved[12];
    int32_t pad1;
};

struct Delete {
    int32_t userId;
    int32_t pad0;
    const char* titleId;
    const DirName* dirName;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad1;
};

int PS4_SYSV_ABI sceSaveDataInitialize(const void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceSaveDataTerminate() { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceSaveDataMount(const Mount* mount, MountResult* result) {
    if (!mount || !mount->dirName || !result) return kErrorParameter;
    const std::string dir = Bounded(mount->dirName->data, kDirNameSize);
    if (dir.empty()) return kErrorParameter;
    std::fprintf(stderr, "[savedata] mount '%s' mode 0x%x blocks %llu\n", dir.c_str(), mount->mountMode,
                 static_cast<unsigned long long>(mount->blocks));
    const fs::path folder = SaveFolder(mount->titleId, dir);
    std::error_code ec;

    std::memset(result, 0, sizeof(*result));
    const bool exists = fs::is_directory(folder, ec);
    const bool create = mount->mountMode & (kCreate | kCreate2);
    if (!exists && !create) return kErrorNotFound;
    if (exists && (mount->mountMode & kCreate)) return kErrorExists;
    if (!exists) {
        fs::create_directories(folder / "sce_sys", ec);
        if (ec) return kErrorInternal;
        result->mountStatus = kCreated;
    }

    std::lock_guard lock(g_mutex);
    for (int slot = 0; slot < kMaxMounts; ++slot) {
        if (!g_mounted[slot].empty()) continue;
        g_mounted[slot] = dir;
        const std::string point = "/savedata" + std::to_string(slot);
        core::vfs::Mount(point, folder, !(mount->mountMode & kRdOnly) || (mount->mountMode & kRdWr));
        std::snprintf(result->mountPoint.data, kMountPointSize, "%s", point.c_str());
        std::fprintf(stderr, "[savedata] mounted '%s' at %s%s\n", dir.c_str(), point.c_str(),
                     result->mountStatus == kCreated ? " (new)" : "");
        return kernel::SCE_OK;
    }
    return kErrorMountFull;
}

int PS4_SYSV_ABI sceSaveDataUmount(const MountPoint* point) {
    if (!point) return kErrorParameter;
    const std::string p = Bounded(point->data, kMountPointSize);
    std::fprintf(stderr, "[savedata] umount %s\n", p.c_str());
    if (!core::vfs::IsMounted(p)) return kErrorNotMounted;
    core::vfs::Unmount(p);
    std::lock_guard lock(g_mutex);
    if (p.rfind("/savedata", 0) == 0) {
        const int slot = std::atoi(p.c_str() + 9);
        if (slot >= 0 && slot < kMaxMounts) g_mounted[slot].clear();
    }
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceSaveDataUmountWithBackup(const MountPoint* point) { return sceSaveDataUmount(point); }

// '%' in a pattern matches any run of characters, like the console.
static bool Matches(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return true;
    const size_t pct = pattern.find('%');
    if (pct == std::string::npos) return name == pattern;
    return name.compare(0, pct, pattern, 0, pct) == 0;
}

int PS4_SYSV_ABI sceSaveDataDirNameSearch(const DirNameSearchCond* cond, DirNameSearchResult* result) {
    if (!cond || !result) return kErrorParameter;
    const fs::path root = SaveFolder(cond->titleId, "");
    const std::string pattern = cond->dirName ? Bounded(cond->dirName->data, kDirNameSize) : "";
    std::fprintf(stderr, "[savedata] search '%s'\n", pattern.c_str());
    std::vector<std::string> hits;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        std::u8string n8 = e.path().filename().u8string();
        std::string name(n8.begin(), n8.end());
        if (Matches(name, pattern)) hits.push_back(name);
    }
    result->hitNum = static_cast<uint32_t>(hits.size());
    result->setNum = 0;
    for (size_t i = 0; i < hits.size() && result->dirNames && i < result->dirNamesNum; ++i) {
        std::snprintf(result->dirNames[i].data, kDirNameSize, "%s", hits[i].c_str());
        ++result->setNum;
    }
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceSaveDataDelete(const Delete* del) {
    if (!del || !del->dirName) return kErrorParameter;
    const fs::path folder = SaveFolder(del->titleId, Bounded(del->dirName->data, kDirNameSize));
    std::error_code ec;
    if (!fs::exists(folder, ec)) return kErrorNotFound;
    fs::remove_all(folder, ec);
    return ec ? kErrorInternal : kernel::SCE_OK;
}

// Titles, icons and backups aren't kept yet; accepting them is harmless.
int PS4_SYSV_ABI sceSaveDataSetParam(const MountPoint* point, uint32_t type, const void*, size_t size) {
    std::fprintf(stderr, "[savedata] set param %u (%zu bytes) on %s\n", type, size,
                 point ? Bounded(point->data, kMountPointSize).c_str() : "?");
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceSaveDataSaveIcon(const MountPoint* point, const void*) {
    std::fprintf(stderr, "[savedata] save icon on %s\n", point ? Bounded(point->data, kMountPointSize).c_str() : "?");
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceSaveDataCheckBackupData(const void*) { return kErrorNotFound; }
int PS4_SYSV_ABI sceSaveDataRestoreBackupData(const void*) { return kErrorNotFound; }

}  // namespace hle::savedata

namespace hle {

void RegisterSaveData(core::Linker& linker) {
    using namespace savedata;
    const char* lib = "libSceSaveData";
    HLE_REG(linker, lib, sceSaveDataInitialize);
    HLE_REG(linker, lib, sceSaveDataTerminate);
    HLE_REG(linker, lib, sceSaveDataMount);
    HLE_REG(linker, lib, sceSaveDataUmount);
    HLE_REG(linker, lib, sceSaveDataUmountWithBackup);
    HLE_REG(linker, lib, sceSaveDataDirNameSearch);
    HLE_REG(linker, lib, sceSaveDataDelete);
    HLE_REG(linker, lib, sceSaveDataSetParam);
    HLE_REG(linker, lib, sceSaveDataSaveIcon);
    HLE_REG(linker, lib, sceSaveDataCheckBackupData);
    HLE_REG(linker, lib, sceSaveDataRestoreBackupData);
}

}  // namespace hle
