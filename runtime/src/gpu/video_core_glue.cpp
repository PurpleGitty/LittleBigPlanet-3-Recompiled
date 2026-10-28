// Stand-ins for the emulator pieces shadPS4's video_core links against
// (memory manager, address space, debug state, clocks, RenderDoc, threads).
#include <map>
#include <shared_mutex>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common/path_util.h"
#include "core/address_space.h"
#include "core/debug_state.h"
#include "core/memory.h"

// --- memory ---------------------------------------------------------------------
// Game memory is plain host memory here (see hle/kernel_memory.cpp), so the
// backing store is the memory itself.

namespace Core {

struct AddressSpace::Impl {};

AddressSpace::AddressSpace() = default;
AddressSpace::~AddressSpace() = default;

// Write tracking: the GPU caches protect pages it has uploaded, and the first
// CPU write faults into the page manager, which marks the cache dirty and
// unprotects. Our HLE file reads go through a bounce buffer (kernel_fs.cpp)
// because ReadFile into a protected page fails instead of faulting.
void AddressSpace::Protect(VAddr addr, u64 size, MemoryPermission perms) {
    DWORD prot = PAGE_NOACCESS;
    const bool r = True(perms & MemoryPermission::Read), w = True(perms & MemoryPermission::Write),
               x = True(perms & MemoryPermission::Execute);
    if (x) prot = w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    else if (w) prot = PAGE_READWRITE;
    else if (r) prot = PAGE_READONLY;
    // The range can span several allocations; VirtualProtect can't, so go region by region.
    const VAddr end = addr + size;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
        const VAddr regionEnd = reinterpret_cast<VAddr>(mbi.BaseAddress) + mbi.RegionSize;
        const VAddr chunkEnd = regionEnd < end ? regionEnd : end;
        if (mbi.State == MEM_COMMIT) {
            DWORD old;
            ::VirtualProtect(reinterpret_cast<void*>(addr), chunkEnd - addr, prot, &old);
        }
        addr = chunkEnd;
    }
}

MemoryManager::MemoryManager() = default;
MemoryManager::~MemoryManager() = default;

// Committed regions seen by VirtualQuery, start -> end. The buffer cache asks
// about thousands of ranges per frame and VirtualQuery is a system call, which
// made it most of the GPU thread's time. Cleared whenever the game maps or
// unmaps memory (gpu::MapMemory / UnmapMemory).
namespace {
std::shared_mutex g_committedMutex;
std::map<VAddr, VAddr> g_committed;
}  // namespace

void ClearCommittedRangeCache() {
    std::unique_lock lock(g_committedMutex);
    g_committed.clear();
}

// Bytes from `addr` that are committed memory, up to `size`.
u64 MemoryManager::ClampRangeSize(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    VAddr at = addr;
    {
        std::shared_lock lock(g_committedMutex);
        while (at < end) {
            auto it = g_committed.upper_bound(at);
            if (it == g_committed.begin()) break;
            --it;
            if (it->second <= at) break;
            at = it->second;
        }
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!::VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        const VAddr base = reinterpret_cast<VAddr>(mbi.BaseAddress);
        const VAddr regionEnd = base + mbi.RegionSize;
        {
            std::unique_lock lock(g_committedMutex);
            g_committed[base] = regionEnd;
        }
        at = regionEnd;
    }
    return (at < end ? at : end) - addr;
}

void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    const u64 readable = ClampRangeSize(source, size);
    std::memcpy(dest, reinterpret_cast<const void*>(source), readable);
    if (readable < size) std::memset(dest + readable, 0, size - readable);
}

// The GPU thread also writes small values here all the time (fences, event
// writes), so this must be a plain copy: WriteProcessMemory made each one a
// system call, a quarter of the GPU thread's time. A write into a page the page
// manager protects faults into its handler on this (GPU) thread, which handles
// it inline; that used to deadlock only because the fault dispatcher held a
// global lock while handlers ran (fixed in shadps4_glue.cpp).
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    std::memcpy(address, data, size);
    return true;
}

}  // namespace Core

// --- debug state (shadPS4's frame/regs dump tooling): unused ----------------------

static DebugStateType::DebugStateImpl g_debugState;
DebugStateType::DebugStateImpl& DebugState = g_debugState;

namespace DebugStateType {
void DebugStateImpl::PushRegsDump(uintptr_t, uintptr_t, const AmdGpu::Regs&) {}
void DebugStateImpl::PushRegsDumpCompute(uintptr_t, uintptr_t, const AmdGpu::ComputeProgram&) {}
}  // namespace DebugStateType

// --- clocks ---------------------------------------------------------------------------

namespace Libraries::Kernel {
u64 sceKernelGetTscFrequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<u64>(f.QuadPart);
}
u64 sceKernelReadTsc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<u64>(c.QuadPart);
}
}  // namespace Libraries::Kernel

namespace Libraries::GnmDriver {
u32 sceGnmGetGpuCoreClockFrequency() { return 800'000'000; }  // PS4 GPU clock
}  // namespace Libraries::GnmDriver

// --- RenderDoc capture hooks: no RenderDoc integration ----------------------------------

namespace VideoCore {
void StartCapture() {}
void EndCapture() {}
}  // namespace VideoCore

// --- misc common/ helpers ---------------------------------------------------------------

namespace Common {

void SetCurrentThreadName(const char* name) {
    std::wstring w(name, name + std::strlen(name));
    SetThreadDescription(GetCurrentThread(), w.c_str());
}

namespace FS {
std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}
}  // namespace FS

}  // namespace Common
