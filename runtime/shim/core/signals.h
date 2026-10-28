// Stand-in for shadPS4's core/signals.h: the recompiler's SRT walker registers
// an access-violation handler; ours is forwarded to a Windows vectored handler.
#pragma once
#include "common/types.h"

namespace Core {

using AccessViolationHandler = bool (*)(void* context, void* fault_address);

class Signals {
public:
    static Signals* Instance();
    void RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32 priority);
};

}  // namespace Core
