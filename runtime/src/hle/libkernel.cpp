#include "hle/libkernel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#include "core/runner.h"
#include "core/tls.h"

namespace hle::kernel {
namespace {

void* const* g_heapApi = nullptr;

}  // namespace

void* PS4_SYSV_ABI __tls_get_addr(const TlsIndex* index) {
    return core::TlsAddress(index->module, index->offset);
}

void* PS4_SYSV_ABI sceKernelGetProcParam() { return core::ProcParam(); }

int* PS4_SYSV_ABI __error() {
    thread_local int err = 0;
    return &err;
}

void PS4_SYSV_ABI _sceKernelRtldSetApplicationHeapAPI(void* const* api) {
    // libc hands over its malloc/free family; we'll use it for TLS blocks of
    // new threads like the PS4 does. Nothing needs it yet.
    g_heapApi = api;
}

void PS4_SYSV_ABI __stack_chk_fail() {
    std::fprintf(stderr, "[kernel] stack smashing detected in game code\n");
    std::fflush(stderr);
    ExitProcess(4);
}

}  // namespace hle::kernel
