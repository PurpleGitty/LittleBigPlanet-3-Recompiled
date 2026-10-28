// libScePlayGo ("play while installing"): everything is installed locally,
// so every chunk reports as present and there's nothing left to download.
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::playgo {
namespace {

constexpr uint32_t kHandle = 1;
constexpr int kErrorBadHandle = static_cast<int>(0x80B20003);
constexpr int kErrorBadPointer = static_cast<int>(0x80B20004);
constexpr int kErrorBadSize = static_cast<int>(0x80B20005);
constexpr int8_t kLocusLocalFast = 3;
enum InstallSpeed : int32_t { kSuspended = 0, kTrickle = 1, kFull = 2 };

int32_t g_speed = kFull;

}  // namespace

struct Progress { uint64_t progressSize, totalSize; };

int PS4_SYSV_ABI scePlayGoInitialize(const void*) { return kernel::SCE_OK; }

int PS4_SYSV_ABI scePlayGoOpen(uint32_t* handle, const void*) {
    if (!handle) return kErrorBadPointer;
    *handle = kHandle;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI scePlayGoClose(uint32_t handle) { return handle == kHandle ? kernel::SCE_OK : kErrorBadHandle; }

int PS4_SYSV_ABI scePlayGoGetLocus(uint32_t handle, const uint16_t* chunks, uint32_t count, int8_t* loci) {
    if (handle != kHandle) return kErrorBadHandle;
    if (!chunks || !loci) return kErrorBadPointer;
    if (count == 0) return kErrorBadSize;
    for (uint32_t i = 0; i < count; ++i) loci[i] = kLocusLocalFast;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI scePlayGoGetProgress(uint32_t handle, const uint16_t* chunks, uint32_t count, Progress* out) {
    if (handle != kHandle) return kErrorBadHandle;
    if (!chunks || !out) return kErrorBadPointer;
    if (count == 0) return kErrorBadSize;
    out->progressSize = out->totalSize = count;  // complete
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI scePlayGoGetToDoList(uint32_t handle, void* todo, uint32_t count, uint32_t* entries) {
    if (handle != kHandle) return kErrorBadHandle;
    if (!todo || !entries) return kErrorBadPointer;
    if (count == 0) return kErrorBadSize;
    *entries = 0;  // nothing to do
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI scePlayGoGetInstallSpeed(uint32_t handle, int32_t* speed) {
    if (handle != kHandle) return kErrorBadHandle;
    if (!speed) return kErrorBadPointer;
    *speed = g_speed;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI scePlayGoSetInstallSpeed(uint32_t handle, int32_t speed) {
    if (handle != kHandle) return kErrorBadHandle;
    g_speed = speed;
    return kernel::SCE_OK;
}

}  // namespace hle::playgo

namespace hle {

void RegisterPlayGo(core::Linker& linker) {
    using namespace playgo;
    const char* lib = "libScePlayGo";
    HLE_REG(linker, lib, scePlayGoInitialize);
    HLE_REG(linker, lib, scePlayGoOpen);
    HLE_REG(linker, lib, scePlayGoClose);
    HLE_REG(linker, lib, scePlayGoGetLocus);
    HLE_REG(linker, lib, scePlayGoGetProgress);
    HLE_REG(linker, lib, scePlayGoGetToDoList);
    HLE_REG(linker, lib, scePlayGoGetInstallSpeed);
    HLE_REG(linker, lib, scePlayGoSetInstallSpeed);
}

}  // namespace hle
