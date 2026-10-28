// libSceNpTrophy, locally: contexts and handles always work, nothing is
// unlocked yet, and unlocks are logged (they could be kept in user/ later).
// The trophy set itself (sce_sys/trophy/trophy00.trp) is encrypted, so the
// trophy count is reported as 0.
#include <atomic>
#include <cstdio>
#include <cstring>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::trophy {
namespace {

constexpr int kErrorInvalidArgument = static_cast<int>(0x80551604);
constexpr int32_t kInvalidTrophyId = -1;
std::atomic<int32_t> g_nextId{1};

struct FlagArray {
    uint32_t bits[128 / 32];
};

}  // namespace

int PS4_SYSV_ABI sceNpTrophyCreateContext(int32_t* context, int32_t, uint32_t, uint64_t) {
    if (!context) return kErrorInvalidArgument;
    *context = g_nextId++;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpTrophyCreateHandle(int32_t* handle) {
    if (!handle) return kErrorInvalidArgument;
    *handle = g_nextId++;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpTrophyDestroyContext(int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpTrophyDestroyHandle(int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpTrophyRegisterContext(int32_t, int32_t, uint64_t) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceNpTrophyGetTrophyUnlockState(int32_t, int32_t, FlagArray* flags, uint32_t* count) {
    if (!flags || !count) return kErrorInvalidArgument;
    std::memset(flags, 0, sizeof(*flags));
    *count = 0;
    return kernel::SCE_OK;
}

// SceNpTrophyDetails / SceNpTrophyData begin with a size_t the caller fills in;
// clear the rest (up to that size) to report an empty, locked trophy.
void ClearSized(void* p, int32_t trophyId) {
    if (!p) return;
    const uint64_t size = *static_cast<uint64_t*>(p);
    if (size <= 8 || size > 4096) return;
    std::memset(static_cast<uint8_t*>(p) + 8, 0, size - 8);
    if (size >= 12) std::memcpy(static_cast<uint8_t*>(p) + 8, &trophyId, 4);  // trophyId follows size
}

int PS4_SYSV_ABI sceNpTrophyGetTrophyInfo(int32_t, int32_t, int32_t trophyId, void* details, void* data) {
    ClearSized(details, trophyId);
    ClearSized(data, trophyId);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpTrophyUnlockTrophy(int32_t, int32_t, int32_t trophyId, int32_t* platinumId) {
    std::fprintf(stderr, "[trophy] unlocked trophy %d\n", trophyId);
    if (platinumId) *platinumId = kInvalidTrophyId;
    return kernel::SCE_OK;
}

}  // namespace hle::trophy

namespace hle {

void RegisterTrophy(core::Linker& linker) {
    using namespace trophy;
    const char* lib = "libSceNpTrophy";
    HLE_REG(linker, lib, sceNpTrophyCreateContext);
    HLE_REG(linker, lib, sceNpTrophyCreateHandle);
    HLE_REG(linker, lib, sceNpTrophyDestroyContext);
    HLE_REG(linker, lib, sceNpTrophyDestroyHandle);
    HLE_REG(linker, lib, sceNpTrophyRegisterContext);
    HLE_REG(linker, lib, sceNpTrophyGetTrophyUnlockState);
    HLE_REG(linker, lib, sceNpTrophyGetTrophyInfo);
    HLE_REG(linker, lib, sceNpTrophyUnlockTrophy);
}

}  // namespace hle
