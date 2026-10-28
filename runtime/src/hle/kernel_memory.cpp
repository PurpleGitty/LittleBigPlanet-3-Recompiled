// libkernel memory: direct (physical) memory, flexible memory, mapping.
//
// First version: "physical" direct memory is just a range of offsets handed out
// by a bump allocator; each map gets fresh host memory. Mapping the same
// physical range twice (aliasing) isn't supported yet and is logged.
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gpu/renderer.h"
#include "hle/kernel_errors.h"
#include "hle/libkernel.h"

namespace hle::kernel {
namespace {

constexpr int SCE_KERNEL_ERROR_ENOMEM = static_cast<int>(0x8002000C);
constexpr int SCE_KERNEL_ERROR_EINVAL = static_cast<int>(0x80020016);

constexpr uint64_t kDirectMemorySize = 0x13C000000;  // 5056 MiB, what retail games see
constexpr uint64_t kPageSize = 16 * 1024;              // PS4 page size
constexpr int kMapFixed = 0x10;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

std::mutex g_mutex;
std::map<uint64_t, uint64_t> g_allocated;  // phys start -> length
std::map<uint64_t, uint64_t> g_mapped;     // phys start -> host address

// PS4 programs live below 0xFC_0000_0000 and libc's allocators reject memory
// above it, so every game-visible mapping is placed inside this window.
constexpr uint64_t kGameSpaceLow = 0x1'0000'0000;
constexpr uint64_t kGameSpaceHigh = 0xFB'FFFF'FFFF;
constexpr uint64_t kWinAllocGranularity = 64 * 1024;

// Host memory for a game mapping. Honors a requested address when given.
void* MapHost(void* want, uint64_t len, uint64_t alignment, bool fixed) {
    if (want) {
        if (void* p = VirtualAlloc(want, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        if (fixed) return nullptr;
    }
    MEM_ADDRESS_REQUIREMENTS req{};
    req.LowestStartingAddress = reinterpret_cast<void*>(kGameSpaceLow);
    req.HighestEndingAddress = reinterpret_cast<void*>(kGameSpaceHigh);
    // VirtualAlloc2 wants a power-of-two alignment of at least 64 KB, or 0.
    req.Alignment = alignment > kWinAllocGranularity && (alignment & (alignment - 1)) == 0 ? alignment : 0;
    MEM_EXTENDED_PARAMETER param{};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;
    return VirtualAlloc2(GetCurrentProcess(), nullptr, AlignUp(len, kWinAllocGranularity), MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE, &param, 1);
}

}  // namespace

uint64_t PS4_SYSV_ABI sceKernelGetDirectMemorySize() { return kDirectMemorySize; }

int PS4_SYSV_ABI sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, uint64_t len,
                                               uint64_t alignment, int32_t memoryType, int64_t* physAddrOut) {
    (void)memoryType;
    if (searchStart < 0 || searchEnd <= searchStart || len == 0 || len % kPageSize || !physAddrOut)
        return SCE_KERNEL_ERROR_EINVAL;
    alignment = alignment ? AlignUp(alignment, kPageSize) : kPageSize;

    std::lock_guard lock(g_mutex);
    // First fit between existing allocations.
    uint64_t cursor = AlignUp(static_cast<uint64_t>(searchStart), alignment);
    for (const auto& [start, length] : g_allocated) {
        if (start + length <= cursor) continue;
        if (cursor + len <= start) break;
        cursor = AlignUp(start + length, alignment);
    }
    if (cursor + len > static_cast<uint64_t>(searchEnd)) return SCE_KERNEL_ERROR_ENOMEM;
    g_allocated[cursor] = len;
    *physAddrOut = static_cast<int64_t>(cursor);
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelAllocateMainDirectMemory(uint64_t len, uint64_t alignment, int32_t memoryType,
                                                   int64_t* physAddrOut) {
    return sceKernelAllocateDirectMemory(0, kDirectMemorySize, len, alignment, memoryType, physAddrOut);
}

int PS4_SYSV_ABI sceKernelReleaseDirectMemory(int64_t start, uint64_t len) {
    (void)len;
    std::lock_guard lock(g_mutex);
    g_allocated.erase(static_cast<uint64_t>(start));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelMapNamedDirectMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                               int64_t physAddr, uint64_t alignment, const char* name) {
    (void)prot;
    if (!addr || len == 0 || len % kPageSize) return SCE_KERNEL_ERROR_EINVAL;
    {
        std::lock_guard lock(g_mutex);
        if (g_mapped.contains(static_cast<uint64_t>(physAddr)))
            std::fprintf(stderr, "[kernel] TODO: direct memory 0x%llx mapped twice (%s)\n",
                         static_cast<unsigned long long>(physAddr), name ? name : "");
    }
    void* p = MapHost(*addr, len, alignment, flags & kMapFixed);
    std::fprintf(stderr, "[kernel] map direct '%s' %llu MB (phys 0x%llx, align 0x%llx) -> %p\n", name ? name : "",
                 static_cast<unsigned long long>(len >> 20), static_cast<unsigned long long>(physAddr),
                 static_cast<unsigned long long>(alignment), p);
    if (!p) return SCE_KERNEL_ERROR_ENOMEM;
    std::lock_guard lock(g_mutex);
    g_mapped[static_cast<uint64_t>(physAddr)] = reinterpret_cast<uint64_t>(p);
    gpu::MapMemory(reinterpret_cast<uint64_t>(p), len);
    *addr = p;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelMapDirectMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                          int64_t physAddr, uint64_t alignment) {
    return sceKernelMapNamedDirectMemory(addr, len, prot, flags, physAddr, alignment, "anon");
}

int PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                                 const char* name) {
    (void)prot;
    (void)name;
    if (!addr || len == 0 || len % kPageSize) return SCE_KERNEL_ERROR_EINVAL;
    void* p = MapHost(*addr, len, kPageSize, flags & kMapFixed);
    if (!p) return SCE_KERNEL_ERROR_ENOMEM;
    gpu::MapMemory(reinterpret_cast<uint64_t>(p), len);
    *addr = p;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelMapFlexibleMemory(void** addr, uint64_t len, int32_t prot, int32_t flags) {
    return sceKernelMapNamedFlexibleMemory(addr, len, prot, flags, "anon");
}

int PS4_SYSV_ABI sceKernelMunmap(void* addr, uint64_t len) {
    (void)len;
    // TODO: partial unmaps; this only handles whole mappings.
    gpu::UnmapMemory(reinterpret_cast<uint64_t>(addr), len);
    VirtualFree(addr, 0, MEM_RELEASE);
    return SCE_OK;
}

int64_t PS4_SYSV_ABI sceKernelPread(int fd, void* buf, uint64_t len, int64_t off);

// File mappings are copies: writes to MAP_SHARED mappings don't reach the file.
void* PS4_SYSV_ABI mmap(void* addr, uint64_t len, int prot, int flags, int fd, int64_t offset) {
    (void)prot;
    constexpr int kMapAnon = 0x1000;
    void* const kFailed = reinterpret_cast<void*>(-1);
    if (len == 0) { *__error() = 22; return kFailed; }  // EINVAL
    void* p = MapHost(addr, AlignUp(len, kPageSize), kPageSize, flags & kMapFixed);
    if (!p) { *__error() = 12; return kFailed; }  // ENOMEM
    if (!(flags & kMapAnon) && fd >= 0) {
        int64_t got = sceKernelPread(fd, p, len, offset);
        if (got < 0) {
            VirtualFree(p, 0, MEM_RELEASE);
            *__error() = static_cast<int>(static_cast<uint32_t>(got) - 0x80020000u);
            return kFailed;
        }
    }
    gpu::MapMemory(reinterpret_cast<uint64_t>(p), AlignUp(len, kPageSize));
    return p;
}

int PS4_SYSV_ABI munmap(void* addr, uint64_t len) { return sceKernelMunmap(addr, len); }
int PS4_SYSV_ABI msync(void*, uint64_t, int) { return 0; }
int PS4_SYSV_ABI sceKernelMlock(const void*, uint64_t) { return SCE_OK; }

// OrbisVirtualQueryInfo (72 bytes).
struct VirtualQueryInfo {
    uint64_t start, end, offset;
    int32_t protection, memoryType;
    uint8_t flags;  // bit 0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed
    char name[32];  // then padding to 72
};
static_assert(sizeof(VirtualQueryInfo) == 72);

// Describes the mapping at (or with flag 1, the next one after) `addr`.
int PS4_SYSV_ABI sceKernelVirtualQuery(const void* addr, int32_t flags, VirtualQueryInfo* info, uint64_t infoSize) {
    constexpr int32_t kFindNext = 1;
    constexpr int SCE_KERNEL_ERROR_EACCES = static_cast<int>(0x8002000D);
    if (!info || infoSize < sizeof(VirtualQueryInfo)) return SCE_KERNEL_ERROR_EINVAL;
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    MEMORY_BASIC_INFORMATION mbi{};
    for (;;) {
        if (a > kGameSpaceHigh || !VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)))
            return SCE_KERNEL_ERROR_EACCES;
        if (mbi.State == MEM_COMMIT) break;
        if (!(flags & kFindNext)) return SCE_KERNEL_ERROR_EACCES;
        a = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    std::memset(info, 0, sizeof(*info));
    info->start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    info->end = info->start + mbi.RegionSize;
    const bool exec = mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE);
    info->protection = 0x3 | (exec ? 0x4 : 0) | 0x30;  // CPU read/write (+exec), GPU read/write
    info->flags = 1u << 4;                              // committed
    return SCE_OK;
}

}  // namespace hle::kernel
