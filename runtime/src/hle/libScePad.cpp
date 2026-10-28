#include "hle/libScePad.h"

#include <cstring>

#include "input/gamepad.h"

namespace hle::pad {
namespace {

constexpr int SCE_OK = 0;
constexpr int SCE_PAD_ERROR_INVALID_ARG = static_cast<int>(0x80920001);
constexpr int SCE_PAD_ERROR_INVALID_HANDLE = static_cast<int>(0x80920003);
constexpr int SCE_PAD_ERROR_ALREADY_OPENED = static_cast<int>(0x80920004);
constexpr int SCE_PAD_ERROR_NOT_INITIALIZED = static_cast<int>(0x80920005);

constexpr int32_t kTypeStandard = 0;
// Handles are slot + 1 so that 0 is never a valid handle.
constexpr int32_t kHandleBase = 1;

bool g_initialized = false;
int32_t g_slotUser[input::kMaxPads] = {};  // 0 = slot free

int SlotFromHandle(int32_t handle) {
    int slot = handle - kHandleBase;
    if (slot < 0 || slot >= input::kMaxPads || g_slotUser[slot] == 0) return -1;
    return slot;
}

}  // namespace

int PS4_SYSV_ABI scePadInit() {
    input::Init();
    g_initialized = true;
    return SCE_OK;
}

int PS4_SYSV_ABI scePadOpen(int32_t userId, int32_t type, int32_t index, const void*) {
    if (!g_initialized) return SCE_PAD_ERROR_NOT_INITIALIZED;
    if (type != kTypeStandard || index != 0) return SCE_PAD_ERROR_INVALID_ARG;
    // Users are numbered from 1 by our libSceUserService; user N gets pad slot N-1.
    int slot = userId - 1;
    if (slot < 0 || slot >= input::kMaxPads) return SCE_PAD_ERROR_INVALID_ARG;
    if (g_slotUser[slot] != 0) return SCE_PAD_ERROR_ALREADY_OPENED;
    g_slotUser[slot] = userId;
    return slot + kHandleBase;
}

int PS4_SYSV_ABI scePadClose(int32_t handle) {
    int slot = SlotFromHandle(handle);
    if (slot < 0) return SCE_PAD_ERROR_INVALID_HANDLE;
    g_slotUser[slot] = 0;
    return SCE_OK;
}

int PS4_SYSV_ABI scePadReadState(int32_t handle, ps4::PadData* data) {
    int slot = SlotFromHandle(handle);
    if (slot < 0) return SCE_PAD_ERROR_INVALID_HANDLE;
    if (!data) return SCE_PAD_ERROR_INVALID_ARG;
    input::Read(slot, *data);  // a disconnected pad is reported via data->connected
    return SCE_OK;
}

int PS4_SYSV_ABI scePadGetControllerInformation(int32_t handle, ps4::PadControllerInformation* info) {
    int slot = SlotFromHandle(handle);
    if (slot < 0) return SCE_PAD_ERROR_INVALID_HANDLE;
    if (!info) return SCE_PAD_ERROR_INVALID_ARG;
    ps4::PadData d;
    bool connected = input::Read(slot, d);
    std::memset(info, 0, sizeof(*info));
    info->touchPixelDensity = 44.86f;  // DualShock 4 touch pad values
    info->touchResX = 1920;
    info->touchResY = 943;
    info->deadZoneLeft = info->deadZoneRight = 13;
    info->connectionType = 0;  // local
    info->connected = connected;
    info->connectedCount = connected ? 1 : 0;
    info->deviceClass = 0;  // standard controller
    return SCE_OK;
}

int PS4_SYSV_ABI scePadSetLightBar(int32_t handle, const ps4::PadLightBarParam* param) {
    int slot = SlotFromHandle(handle);
    if (slot < 0) return SCE_PAD_ERROR_INVALID_HANDLE;
    if (!param) return SCE_PAD_ERROR_INVALID_ARG;
    input::SetLightBar(slot, param->r, param->g, param->b);
    return SCE_OK;
}

int PS4_SYSV_ABI scePadSetVibration(int32_t handle, const ps4::PadVibrationParam* param) {
    int slot = SlotFromHandle(handle);
    if (slot < 0) return SCE_PAD_ERROR_INVALID_HANDLE;
    if (!param) return SCE_PAD_ERROR_INVALID_ARG;
    input::SetVibration(slot, param->largeMotor, param->smallMotor);
    return SCE_OK;
}

}  // namespace hle::pad
