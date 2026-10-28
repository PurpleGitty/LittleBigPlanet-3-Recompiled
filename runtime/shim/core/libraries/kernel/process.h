// Stand-in for shadPS4's core/libraries/kernel/process.h: only the PS4 Pro
// check is used (by the command processor). We present as a base PS4.
#pragma once
#include "common/types.h"

namespace Libraries::Kernel {
inline s32 sceKernelIsNeoMode() { return 0; }
}  // namespace Libraries::Kernel
