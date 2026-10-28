// Call counts for the game script language's native functions (LBP3_COUNT_NATIVES=1).
#pragma once

#include "hle/abi.h"

namespace core {

class Module;

// Wraps every native in the executable's binding tables with a call counter.
void InstallNativeCounters(Module& m);

// Script natives are called as fn(self, return slot, argument array).
using ScriptNative = void(PS4_SYSV_ABI*)(void* self, void* ret, const unsigned long long* args);
// Points the binding table entry for `name` (e.g. "RequestHostUsePod__b") at
// `replacement`; returns the previous function, or nullptr if the name isn't found.
ScriptNative HookNative(Module& m, const char* name, ScriptNative replacement);
// Prints the most-called natives since the last report and resets the counts.
void PrintNativeCounts(int top);

}  // namespace core
