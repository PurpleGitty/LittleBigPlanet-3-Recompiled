// HLE replacement for libScePad: the 7 functions LittleBigPlanet 3 imports.
#pragma once
#include "hle/abi.h"
#include "hle/pad_types.h"

namespace hle::pad {

int PS4_SYSV_ABI scePadInit();
int PS4_SYSV_ABI scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
int PS4_SYSV_ABI scePadClose(int32_t handle);
int PS4_SYSV_ABI scePadReadState(int32_t handle, ps4::PadData* data);
int PS4_SYSV_ABI scePadGetControllerInformation(int32_t handle, ps4::PadControllerInformation* info);
int PS4_SYSV_ABI scePadSetLightBar(int32_t handle, const ps4::PadLightBarParam* param);
int PS4_SYSV_ABI scePadSetVibration(int32_t handle, const ps4::PadVibrationParam* param);

}  // namespace hle::pad
