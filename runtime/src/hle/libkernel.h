// HLE replacement for libkernel. Grows as the game asks for more.
#pragma once
#include <cstdint>

#include "hle/abi.h"

namespace hle::kernel {

struct TlsIndex { uint64_t module, offset; };

void* PS4_SYSV_ABI __tls_get_addr(const TlsIndex* index);
[[noreturn]] void PS4_SYSV_ABI __stack_chk_fail();

// kernel_memory.cpp
uint64_t PS4_SYSV_ABI sceKernelGetDirectMemorySize();
int PS4_SYSV_ABI sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, uint64_t len,
                                               uint64_t alignment, int32_t memoryType, int64_t* physAddrOut);
int PS4_SYSV_ABI sceKernelAllocateMainDirectMemory(uint64_t len, uint64_t alignment, int32_t memoryType,
                                                   int64_t* physAddrOut);
int PS4_SYSV_ABI sceKernelReleaseDirectMemory(int64_t start, uint64_t len);
int PS4_SYSV_ABI sceKernelMapNamedDirectMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                               int64_t physAddr, uint64_t alignment, const char* name);
int PS4_SYSV_ABI sceKernelMapDirectMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                          int64_t physAddr, uint64_t alignment);
int PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr, uint64_t len, int32_t prot, int32_t flags,
                                                 const char* name);
int PS4_SYSV_ABI sceKernelMapFlexibleMemory(void** addr, uint64_t len, int32_t prot, int32_t flags);
struct VirtualQueryInfo;
int PS4_SYSV_ABI sceKernelVirtualQuery(const void* addr, int32_t flags, VirtualQueryInfo* info, uint64_t infoSize);
int PS4_SYSV_ABI sceKernelMunmap(void* addr, uint64_t len);
void* PS4_SYSV_ABI mmap(void* addr, uint64_t len, int prot, int flags, int fd, int64_t offset);
int PS4_SYSV_ABI munmap(void* addr, uint64_t len);
int PS4_SYSV_ABI msync(void* addr, uint64_t len, int flags);
int PS4_SYSV_ABI sceKernelMlock(const void* addr, uint64_t len);

// libkernel.cpp
void* PS4_SYSV_ABI sceKernelGetProcParam();
int* PS4_SYSV_ABI __error();
void PS4_SYSV_ABI _sceKernelRtldSetApplicationHeapAPI(void* const* api);

}  // namespace hle::kernel
