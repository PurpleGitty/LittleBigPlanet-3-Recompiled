// libSceUserService and libSceSystemService: one local player who is always
// signed in, and console settings that match a stock English PS4.
#include <atomic>
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::user {
namespace {

constexpr int32_t kUserId = 1;  // our libScePad maps user N to pad slot N-1
constexpr int32_t kInvalidUser = -1;
constexpr int kMaxLoginUsers = 4;
constexpr int kErrorNoEvent = static_cast<int>(0x80960007);
constexpr int kErrorInvalidArgument = static_cast<int>(0x80960002);

std::atomic<bool> g_loginReported{false};

}  // namespace

struct LoginUserIdList { int32_t userId[kMaxLoginUsers]; };
struct UserEvent { int32_t eventType; int32_t userId; };  // 0 = login, 1 = logout

int PS4_SYSV_ABI sceUserServiceInitialize(const void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceUserServiceTerminate() { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceUserServiceGetInitialUser(int32_t* userId) {
    if (!userId) return kErrorInvalidArgument;
    *userId = kUserId;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceUserServiceGetLoginUserIdList(LoginUserIdList* list) {
    if (!list) return kErrorInvalidArgument;
    list->userId[0] = kUserId;
    for (int i = 1; i < kMaxLoginUsers; ++i) list->userId[i] = kInvalidUser;
    return kernel::SCE_OK;
}

// One login event for our player, then the queue is empty.
int PS4_SYSV_ABI sceUserServiceGetEvent(UserEvent* ev) {
    if (!ev) return kErrorInvalidArgument;
    if (g_loginReported.exchange(true)) return kErrorNoEvent;
    *ev = {0, kUserId};
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceUserServiceGetUserName(int32_t userId, char* name, size_t size) {
    if (!name || size == 0) return kErrorInvalidArgument;
    (void)userId;
    char buf[64] = "Player";
    DWORD len = sizeof(buf);
    GetUserNameA(buf, &len);  // the Windows account name makes a friendly default
    std::snprintf(name, size, "%s", buf);
    return kernel::SCE_OK;
}

}  // namespace hle::user

namespace hle::system {
namespace {

enum ParamId : int32_t {
    kParamLang = 1, kParamDateFormat = 2, kParamTimeFormat = 3, kParamTimeZone = 4,
    kParamSummertime = 5, kParamGameParentalLevel = 7, kParamEnterButtonAssign = 1000,
};
constexpr int kErrorParameter = static_cast<int>(0x80A10003);
constexpr int32_t kLangEnglishUs = 1;
constexpr int32_t kEnterButtonCross = 1;

}  // namespace

struct SystemServiceStatus {
    int32_t eventNum;
    bool isSystemUiOverlaid;
    bool isInBackgroundExecution;
    bool isCpuMode7CpuNormal;
    bool isGameLiveStreamingOnAir;
    bool isOutOfVrPlayArea;
    uint8_t reserved[125];
};

struct DisplaySafeAreaInfo { float ratio; uint8_t reserved[128]; };

int PS4_SYSV_ABI sceSystemServiceGetStatus(SystemServiceStatus* status) {
    if (!status) return kErrorParameter;
    std::memset(status, 0, sizeof(*status));
    status->isCpuMode7CpuNormal = true;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceSystemServiceGetDisplaySafeAreaInfo(DisplaySafeAreaInfo* info) {
    if (!info) return kErrorParameter;
    std::memset(info, 0, sizeof(*info));
    info->ratio = 1.0f;  // no overscan on a PC monitor
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceSystemServiceParamGetInt(int32_t id, int32_t* value) {
    if (!value) return kErrorParameter;
    switch (id) {
    case kParamLang: *value = kLangEnglishUs; break;
    case kParamDateFormat: *value = 0; break;  // YYYY/MM/DD
    case kParamTimeFormat: *value = 0; break;  // 12 hour
    case kParamTimeZone: {
        TIME_ZONE_INFORMATION tz;
        GetTimeZoneInformation(&tz);
        *value = -static_cast<int32_t>(tz.Bias);  // minutes east of UTC
        break;
    }
    case kParamSummertime: *value = 0; break;
    case kParamGameParentalLevel: *value = 0; break;  // no restriction
    case kParamEnterButtonAssign: *value = kEnterButtonCross; break;
    default:
        std::fprintf(stderr, "[system] TODO: ParamGetInt(%d)\n", id);
        *value = 0;
    }
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceSystemServiceHideSplashScreen() { return kernel::SCE_OK; }

}  // namespace hle::system

namespace hle {

void RegisterSystemServices(core::Linker& linker) {
    using namespace user;
    using namespace system;
    const char* u = "libSceUserService";
    HLE_REG(linker, u, sceUserServiceInitialize);
    HLE_REG(linker, u, sceUserServiceTerminate);
    HLE_REG(linker, u, sceUserServiceGetInitialUser);
    HLE_REG(linker, u, sceUserServiceGetLoginUserIdList);
    HLE_REG(linker, u, sceUserServiceGetEvent);
    HLE_REG(linker, u, sceUserServiceGetUserName);
    const char* s = "libSceSystemService";
    HLE_REG(linker, s, sceSystemServiceGetStatus);
    HLE_REG(linker, s, sceSystemServiceGetDisplaySafeAreaInfo);
    HLE_REG(linker, s, sceSystemServiceParamGetInt);
    HLE_REG(linker, s, sceSystemServiceHideSplashScreen);
}

}  // namespace hle
