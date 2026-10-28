// libSceAppContent: the full game with no add-on content (DLC packages aren't
// part of the dump), and temporary storage at /temp0. Values follow shadPS4.
#include <cstdio>
#include <cstring>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::appcontent {
namespace {

constexpr int kErrorParameter = static_cast<int>(0x80D90002);
constexpr int kErrorNotFound = static_cast<int>(0x80D90005);
constexpr int kErrorDrmNoEntitlement = static_cast<int>(0x80D90007);

enum AppParamId : uint32_t { kSkuFlag = 0, kUserParam1 = 1, kUserParam4 = 4 };
constexpr int32_t kSkuFull = 3;
constexpr size_t kMountPointSize = 16;
constexpr uint64_t kTempSpaceKb = 1024 * 1024;  // 1 GB free

}  // namespace

struct MountPoint { char data[kMountPointSize]; };

int PS4_SYSV_ABI sceAppContentInitialize(const void* initParam, uint32_t* bootParam) {
    (void)initParam;
    if (bootParam) std::memset(bootParam, 0, 0x24);  // OrbisAppContentBootParam; no boot attributes
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAppContentAppParamGetInt(uint32_t id, int32_t* value) {
    if (!value) return kErrorParameter;
    if (id == kSkuFlag) *value = kSkuFull;
    else if (id >= kUserParam1 && id <= kUserParam4) *value = 0;
    else return kErrorParameter;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoList(uint32_t, void*, uint32_t, uint32_t* hitNum) {
    if (hitNum) *hitNum = 0;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfo(uint32_t, const void*, void*) { return kErrorNotFound; }
int PS4_SYSV_ABI sceAppContentGetEntitlementKey(uint32_t, const void*, void*) { return kErrorDrmNoEntitlement; }
int PS4_SYSV_ABI sceAppContentAddcontMount(uint32_t, const void*, MountPoint*) { return kErrorNotFound; }

int PS4_SYSV_ABI sceAppContentTemporaryDataMount2(uint32_t option, MountPoint* mount) {
    (void)option;
    if (!mount) return kErrorParameter;
    std::snprintf(mount->data, kMountPointSize, "/temp0");
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataGetAvailableSpaceKb(const MountPoint*, uint64_t* kb) {
    if (!kb) return kErrorParameter;
    *kb = kTempSpaceKb;
    return kernel::SCE_OK;
}

}  // namespace hle::appcontent

namespace hle {

void RegisterAppContent(core::Linker& linker) {
    using namespace appcontent;
    const char* lib = "libSceAppContent";
    HLE_REG(linker, lib, sceAppContentInitialize);
    HLE_REG(linker, lib, sceAppContentAppParamGetInt);
    HLE_REG(linker, lib, sceAppContentGetAddcontInfoList);
    HLE_REG(linker, lib, sceAppContentGetAddcontInfo);
    HLE_REG(linker, lib, sceAppContentGetEntitlementKey);
    HLE_REG(linker, lib, sceAppContentAddcontMount);
    HLE_REG(linker, lib, sceAppContentTemporaryDataMount2);
    HLE_REG(linker, lib, sceAppContentTemporaryDataGetAvailableSpaceKb);
}

}  // namespace hle
