#include "input/game_state.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "core/native_trace.h"
#include "core/runner.h"
#include "hle/abi.h"

namespace input::game_state {
namespace {

// eboot.bin offsets of the functions the natives wrap (CUSA00473 patch build).
constexpr uint64_t kAnyPoppetActive = 0x979800;    // AnyPoppetActive__
constexpr uint64_t kIsStartMenuOpen = 0x7c3320;    // IsStartMenuOpen__
constexpr uint64_t kPodComputerInUse = 0x6da320;   // GetPodComputerInUse__
constexpr uint64_t kRequestHostUsePod = 0x6da180;  // RequestHostUsePod__b(true): opens the computer

constexpr uint64_t kIsCutSceneLevel = 0x7c5d00;    // IsCutSceneLevel__
constexpr uint64_t kSkipCutSceneLevel = 0x7c5d80;  // SkipCutSceneLevel__ (pause menu "skip")
constexpr uint64_t kGame = 0x12ecc80;              // global game object pointer

bool Ask(uint64_t offset) {
    using Query = bool(PS4_SYSV_ABI*)();
    uint8_t* base = core::EbootBase();
    return base && reinterpret_cast<Query>(base + offset)();
}

// The Pod script opens the computer through RequestHostUsePod__b(true) when Square is
// pressed in reach of it (and calls it with false when the computer closes).
core::ScriptNative g_requestHostUsePod = nullptr;
std::atomic<bool> g_blockComputer{false};

void PS4_SYSV_ABI RequestHostUsePod(void* self, void* ret, const unsigned long long* args) {
    const bool open = args && (args[0] & 0xff);
    if (open && g_blockComputer) return;
    if (g_requestHostUsePod) g_requestHostUsePod(self, ret, args);
}

// Debug (LBP3_TRACE_IS_PLAYING=1): logs GetIsPlaying__Q11AudioHandle results when they
// change per handle; LBP3_TRACE_IS_PLAYING=force reports every sound as stopped.
core::ScriptNative g_getIsPlaying = nullptr;
bool g_forceNotPlaying = false;

void PS4_SYSV_ABI GetIsPlaying(void* self, void* ret, const unsigned long long* args) {
    g_getIsPlaying(self, ret, args);
    static std::atomic<uint64_t> lastSelf{0};
    static std::atomic<int> lastResult{-1};
    const int result = ret ? *static_cast<uint8_t*>(ret) : -1;
    const uint64_t handle = reinterpret_cast<uint64_t>(self);  // the AudioHandle
    if (handle != lastSelf.exchange(handle) || result != lastResult.exchange(result))
        std::fprintf(stderr, "[audio] script GetIsPlaying(handle %llx) = %d%s\n",
                     static_cast<unsigned long long>(handle), result, g_forceNotPlaying ? " (forced 0)" : "");
    if (g_forceNotPlaying && ret) *static_cast<uint8_t*>(ret) = 0;
}

// The world's cutscene camera manager (PWorld + 0x810), found the way IsCutSceneActive__'s
// function (eboot+0x9649b0) does: game -> level (+0x70) -> world thing (+0x68) -> its
// PWorld part, picked from a part table by flag bits. nullptr outside a level.
const uint8_t* CutSceneManager() {
    uint8_t* base = core::EbootBase();
    if (!base) return nullptr;
    const auto q = [](const uint8_t* p, uint64_t off) { return *reinterpret_cast<const uint64_t*>(p + off); };
    const auto* game = reinterpret_cast<const uint8_t*>(q(base, kGame));
    if (!game) return nullptr;
    const auto* level = reinterpret_cast<const uint8_t*>(q(game, 0x70));
    if (!level || (level[0x45] & 0xfe) != 4) return nullptr;
    const auto* thing = reinterpret_cast<const uint8_t*>(q(level, 0x68));
    if (!thing) return nullptr;
    const uint64_t table = q(thing, 0x50) + 0x10;
    const uint64_t inlineParts = reinterpret_cast<uint64_t>(thing) + 2 * (q(thing, 0x20) & 0xc) + 0x58;
    const uint64_t useTable = ((*reinterpret_cast<const uint32_t*>(thing + 0x28) >> 1) & 1) - 1;
    const uint64_t present = 0 - ((q(thing, 0x18) >> 2) & 1);
    const uint64_t pick = useTable & present;
    const uint64_t slot = (pick & table) | (~pick & inlineParts);
    const uint64_t world = present & *reinterpret_cast<const uint64_t*>(slot);
    return world ? reinterpret_cast<const uint8_t*>(world + 0x810) : nullptr;
}

}  // namespace

void CutSceneWatchdog() {
    // The Legend of Bunkum (a cutscene level) never ends by itself here: its last cutscene
    // camera finishes and the level sits on a white screen (cause unknown, see NOTES.md).
    // Pause -> skip works, so once a cutscene level has had a camera and then none for a
    // few seconds, ask for the same skip. LBP3_CUTSCENE_AUTOSKIP=off disables.
    static const bool enabled = [] {
        const char* v = std::getenv("LBP3_CUTSCENE_AUTOSKIP");
        return !v || std::string_view(v) != "off";
    }();
    static const uint8_t* manager = nullptr;
    static bool hadCamera = false;
    static std::chrono::steady_clock::time_point idleSince{};
    static bool skipped = false;
    if (!enabled) return;
    const uint8_t* m = Ask(kIsCutSceneLevel) ? CutSceneManager() : nullptr;
    if (m != manager) {  // another level (or none)
        manager = m;
        hadCamera = false;
        skipped = false;
    }
    if (!m) return;
    const auto now = std::chrono::steady_clock::now();
    const bool camera = *reinterpret_cast<const uint64_t*>(m + 0x8) != 0;  // current cutscene camera
    if (camera && !hadCamera) std::fprintf(stderr, "[cutscene] cutscene level: first camera active\n");
    if (camera || StartMenuOpen()) {
        hadCamera |= camera;
        idleSince = now;
        return;
    }
    if (!hadCamera || skipped) return;
    if (now - idleSince >= std::chrono::seconds(5)) {
        skipped = true;
        std::fprintf(stderr, "[cutscene] last camera finished 5 s ago and the level hasn't ended: skipping it\n");
        using Skip = void(PS4_SYSV_ABI*)();
        reinterpret_cast<Skip>(core::EbootBase() + kSkipCutSceneLevel)();
    }
}

void InstallHooks() {
    if (g_requestHostUsePod) return;
    if (const char* v = std::getenv("LBP3_TRACE_IS_PLAYING")) {
        g_forceNotPlaying = std::string_view(v) == "force";
        if (core::Module* m = core::EbootModule())
            g_getIsPlaying = core::HookNative(*m, "GetIsPlaying__Q11AudioHandle", GetIsPlaying);
    }
    if (core::Module* m = core::EbootModule())
        g_requestHostUsePod = core::HookNative(*m, "RequestHostUsePod__b", RequestHostUsePod);
    if (!g_requestHostUsePod) std::fprintf(stderr, "[input] RequestHostUsePod__b not found\n");
}

void BlockComputerFromSquare(bool block) { g_blockComputer = block; }

bool PopitOpen() { return Ask(kAnyPoppetActive); }
bool StartMenuOpen() { return Ask(kIsStartMenuOpen); }
bool PodComputerInUse() { return Ask(kPodComputerInUse); }

void UsePodComputer() {
    using Request = void(PS4_SYSV_ABI*)(bool);
    if (uint8_t* base = core::EbootBase()) reinterpret_cast<Request>(base + kRequestHostUsePod)(true);
}

}  // namespace input::game_state
