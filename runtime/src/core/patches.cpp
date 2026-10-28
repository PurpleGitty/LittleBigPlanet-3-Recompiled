// Compatibility patches: small, documented byte edits to the game's code,
// applied after relocation and before the code is made read-only. Each patch
// names the bytes it expects and is skipped if they differ (another game
// version), so nothing is ever patched blindly.
#include "core/patches.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/module.h"

namespace core {
namespace {

struct Patch {
    const char* module;
    uint64_t offset;
    std::vector<uint8_t> expect;
    std::vector<uint8_t> replace;
    const char* why;
    const char* onlyWithEnv = nullptr;  // experimental patches apply only when this variable is set
};

const Patch kPatches[] = {
    {"eboot.bin", 0x360b01,
     {0x0f, 0x85, 0xef, 0x44, 0x00, 0x00},              // jne <idle>  (player unchanged)
     {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},              // 6-byte nop
     "network flow 2 / action 3: with LBP3's servers gone the 'connected' messages that "
     "advance this step never arrive, and it idled forever while the player stayed "
     "'OfflinePlayer'. Fall through to the game's own timeout instead, which shows its "
     "'can't connect' message and switches to the offline flow. OBSOLETE (off by default): "
     "since the loopback/identity fixes the local session forms on its own, and skipping this "
     "step loses the host join (flow 2 action 2's message 0), so the first level never started.",
     "LBP3_PATCH_NET_TIMEOUT"},
    {"eboot.bin", 0x360b5b,
     {0x0f, 0x86, 0x18, 0x35, 0x00, 0x00},              // jbe <keep waiting>  (timeout not reached)
     {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},              // 6-byte nop
     "same step: don't sit out the long connect timeout for servers that no longer exist; "
     "give up at once (same message and offline flow as the timeout). OBSOLETE, see above.",
     "LBP3_PATCH_NET_TIMEOUT"},
    {"eboot.bin", 0x36c150,
     {0x55, 0x48, 0x89},                                // push rbp; mov rbp, ...
     {0xb0, 0x01, 0xc3},                                // mov al, 1; ret
     "EXPERIMENT: 'is the local player in the session?' always yes (network flow 16 / "
     "action 20 waits on it before the first level starts).",
     "LBP3_PATCH_SESSION"},
    {"eboot.bin", 0x377050,
     {0x55, 0x48, 0x89},                                // push rbp; mov rbp, ...
     {0xb0, 0x01, 0xc3},                                // mov al, 1; ret
     "EXPERIMENT: 'does the game host have this player as a peer?' always yes, so the pending "
     "session entry is activated (0x3ad9c0) instead of removed (0x34e9f0) in 0x3aea50.",
     "LBP3_PATCH_PEER"},
    {"eboot.bin", 0x3a803e,
     {0x74, 0x05},                                      // je <skip>  (sender != host)
     {0x66, 0x90},                                      // 2-byte nop
     "EXPERIMENT: host message 0x18 from a player not yet in the host table always takes the "
     "'that's me, the host' path (0x3a1ff0). The real check compares through a pointer into the "
     "peer list that 0x3a76e0 has just pruned, so it reads an empty ID.",
     "LBP3_PATCH_SELFHOST"},
    {"eboot.bin", 0x39ffdd,
     {0xbe, 0x03, 0x00, 0x00, 0x00},                    // mov esi, 3  (peer status when host not in table)
     {0xbe, 0x02, 0x00, 0x00, 0x00},                    // mov esi, 2
     "EXPERIMENT: a newly connected peer gets status 2 instead of 3, so the host's table "
     "diff (0x3a76e0) doesn't prune the host's own peer before its first update.",
     "LBP3_PATCH_STATUS2"},
    {"eboot.bin", 0x3ea8c0,
     {0x8b, 0x47, 0x04},                                // mov eax, [rdi+4]  (sound handle is-playing test)
     {0x31, 0xc0, 0xc3},                                // xor eax, eax; ret
     "EXPERIMENT: every sound reports not playing (script native GetIsPlaying); tells whether "
     "the end of The Legend of Bunkum waits on a sound that never stops.",
     "LBP3_PATCH_SOUND_NEVER_PLAYING"},
};

}  // namespace

void ApplyPatches(Module& m) {
    for (const Patch& p : kPatches) {
        if (m.fileName != p.module) continue;
        if (p.onlyWithEnv && !std::getenv(p.onlyWithEnv)) continue;
        if (!p.onlyWithEnv && std::getenv("LBP3_NO_PATCHES")) continue;  // for experiments
        uint8_t* at = m.Addr(p.offset);
        if (std::memcmp(at, p.expect.data(), p.expect.size()) != 0) {
            std::fprintf(stderr, "[patch] %s+0x%llx: bytes differ, skipped\n", p.module,
                         static_cast<unsigned long long>(p.offset));
            continue;
        }
        std::memcpy(at, p.replace.data(), p.replace.size());
        std::fprintf(stderr, "[patch] %s+0x%llx applied\n", p.module, static_cast<unsigned long long>(p.offset));
    }
}

}  // namespace core
