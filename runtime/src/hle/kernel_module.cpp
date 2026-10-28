// libkernel module loading. Every module the game ships is already loaded and
// linked at startup, so "loading" one means finding it and running its init.
#include <cstdio>
#include <cstring>
#include <string>

#include "core/runner.h"
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::kernel {

int PS4_SYSV_ABI sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                                          const void* opt, int* result) {
    (void)flags;
    (void)opt;
    if (!path) return SceErr(kEFAULT);
    const char* slash = std::strrchr(path, '/');
    core::Module* m = core::FindLoadedModule(slash ? slash + 1 : path);
    if (!m) {
        std::fprintf(stderr, "[kernel] LoadStartModule: %s isn't one of the game's modules\n", path);
        return SceErr(kENOENT);
    }
    int ret = core::StartModule(*m, args, argp);
    if (result) *result = ret;
    return core::ModuleHandle(m);
}

}  // namespace hle::kernel

namespace hle {

void RegisterKernelModule(core::Linker& linker) {
    using namespace kernel;
    HLE_REG(linker, "libkernel", sceKernelLoadStartModule);
}

}  // namespace hle
