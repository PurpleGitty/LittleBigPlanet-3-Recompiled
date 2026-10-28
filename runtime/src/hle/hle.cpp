#include "hle/hle.h"

#include "core/linker.h"
#include "hle/libScePad.h"
#include "hle/libkernel.h"
#include "hle/register.h"

namespace hle {

#define REG(lib, fn) linker.RegisterHle(lib, #fn, reinterpret_cast<void*>(&fn))

void RegisterAll(core::Linker& linker) {
    RegisterKernelPthread(linker);
    RegisterKernelSync(linker);
    RegisterKernelTime(linker);
    RegisterKernelFs(linker);
    RegisterKernelModule(linker);
    linker.AddLibraryAlias("libSceLibcInternal", "libc");
    RegisterVideoOut(linker);
    RegisterSystemServices(linker);
    RegisterGnmPackets(linker);
    RegisterAppContent(linker);
    RegisterPlayGo(linker);
    RegisterSaveData(linker);
    RegisterNpOffline(linker);
    RegisterNetOffline(linker);
    RegisterRtc(linker);
    RegisterHttpOffline(linker);
    RegisterDialogs(linker);
    RegisterAudioOut(linker);
    RegisterTrophy(linker);
    RegisterFiber(linker);
    {
        using namespace kernel;
        REG("libkernel", __tls_get_addr);
        REG("libkernel", __stack_chk_fail);
        REG("libkernel", sceKernelGetDirectMemorySize);
        REG("libkernel", sceKernelAllocateDirectMemory);
        REG("libkernel", sceKernelAllocateMainDirectMemory);
        REG("libkernel", sceKernelReleaseDirectMemory);
        REG("libkernel", sceKernelMapNamedDirectMemory);
        REG("libkernel", sceKernelMapDirectMemory);
        REG("libkernel", sceKernelMapNamedFlexibleMemory);
        REG("libkernel", sceKernelMapFlexibleMemory);
        REG("libkernel", sceKernelVirtualQuery);
        REG("libkernel", sceKernelMunmap);
        REG("libkernel", mmap);
        REG("libkernel", munmap);
        REG("libkernel", msync);
        REG("libkernel", sceKernelMlock);
        REG("libkernel", sceKernelGetProcParam);
        REG("libkernel", __error);
        REG("libkernel", _sceKernelRtldSetApplicationHeapAPI);
    }
    {
        using namespace pad;
        REG("libScePad", scePadInit);
        REG("libScePad", scePadOpen);
        REG("libScePad", scePadClose);
        REG("libScePad", scePadReadState);
        REG("libScePad", scePadGetControllerInformation);
        REG("libScePad", scePadSetLightBar);
        REG("libScePad", scePadSetVibration);
    }
}

#undef REG

}  // namespace hle
