// Starts the game: module start functions, then eboot's entry point, on a
// thread with a PS4-sized stack. Crashes are reported in terms of game code.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "core/module.h"

namespace core {

// eboot's PT_SCE_PROCPARAM block (SDK version, heap settings...), or nullptr.
void* ProcParam();

// Loaded module by file name (case-insensitive), or nullptr.
Module* FindLoadedModule(const std::string& fileName);
// 1-based handle for a loaded module, as the game sees it.
int ModuleHandle(const Module* m);
// Runs a module's init once. Returns its result (0 if it has none or already ran).
int StartModule(Module& m, size_t args, const void* argp);

// Lets thread dumps include this thread. Call from the thread itself.
void RegisterCurrentThread(const std::string& name);
void UnregisterCurrentThread();
// "module+0xoffset" for game code, function name for runtime code.
std::string DescribeAddress(uint64_t addr);
// Where eboot.bin is loaded (game function and data offsets are relative to it).
uint8_t* EbootBase();
class Module;
Module* EbootModule();
// Name of a registered thread by Windows thread ID ("?" if unknown).
std::string ThreadName(unsigned long tid);
// Prints where every registered thread currently is in game code.
void DumpAllThreads();
// After `seconds`, dump all threads and exit (0 = never).
void SetDumpAfter(int seconds);

// Samples a thread (game thread name or host thread description) and writes
// where its time goes to `out`.
void SampleThreadReport(const std::string& target, int samples, FILE* out);
// Writes analysis/perf/perfNN.txt: main game thread and GPU thread sampled for
// about 3 seconds, side by side (the F10 hotkey).
void RequestPerfReport();

// Records a call to an unimplemented function for crash reports.
void NoteStubCall(const std::string& lib, const std::string& name);

// Never returns; exits the process when the game exits or crashes.
[[noreturn]] void Run(Module& eboot, const std::vector<Module*>& startFirst,
                      const std::vector<Module*>& allModules);

}  // namespace core
