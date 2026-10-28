// Helpers shared by the HLE libraries.
#pragma once
#include "core/linker.h"
#include "hle/abi.h"

// Registers `fn` under its own name. Functions with POSIX names live in hle
// namespaces, so they never collide with the host C library.
#define HLE_REG(linker, lib, fn) (linker).RegisterHle(lib, #fn, reinterpret_cast<void*>(&fn))

namespace hle {

void RegisterKernelPthread(core::Linker& linker);
void RegisterKernelSync(core::Linker& linker);
void RegisterKernelTime(core::Linker& linker);
void RegisterKernelFs(core::Linker& linker);
void RegisterKernelModule(core::Linker& linker);
void RegisterVideoOut(core::Linker& linker);
void RegisterSystemServices(core::Linker& linker);
void RegisterGnmPackets(core::Linker& linker);
void RegisterAppContent(core::Linker& linker);
void RegisterPlayGo(core::Linker& linker);
void RegisterSaveData(core::Linker& linker);
void RegisterNpOffline(core::Linker& linker);
void RegisterNetOffline(core::Linker& linker);
void RegisterRtc(core::Linker& linker);
void RegisterHttpOffline(core::Linker& linker);
void RegisterDialogs(core::Linker& linker);
void RegisterAudioOut(core::Linker& linker);
void RegisterTrophy(core::Linker& linker);
void RegisterFiber(core::Linker& linker);

}  // namespace hle
