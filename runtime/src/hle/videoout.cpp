// libSceVideoOut + the submit/flip half of libSceGnmDriver, as scaffolding.
//
// There's no window or GPU emulation yet. Command buffers are ignored, but
// every submit reports the GPU events the game registered (end-of-pipe etc.)
// as if the work finished instantly, and flips are paced to 60 Hz and reported
// through flip events. That keeps the game's frame loop running at a real speed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "core/runner.h"
#include "hle/abi.h"
#include "hle/kernel_errors.h"
#include "hle/kernel_sync.h"
#include "hle/pm4.h"
#include "hle/register.h"
#include "gpu/renderer.h"
#include "video/presenter.h"

namespace VideoCore {
extern std::atomic<uint32_t> g_lbp3ImageCreates, g_lbp3ImageExpands;  // texture_cache.cpp
}

namespace hle::kernel {
void NoteFlip(int64_t steadyNs);  // kernel_time.cpp: the main thread's frame-steady clock
void NoteMainThread();
}  // namespace hle::kernel

namespace hle::videoout {

using Clock = std::chrono::steady_clock;

constexpr int kHandle = 1;  // single main display port
// Flip events use an internal ident and pack the flip argument above bit 16.
constexpr uint64_t kInternalFlipEventId = 0x6;
constexpr auto kFrameTime = std::chrono::nanoseconds(1'000'000'000 / 60);
constexpr uint64_t kFlipsPerStatus = 30;  // a status line every ~0.5 s

std::mutex g_mutex;
std::vector<void*> g_flipQueues;
struct GpuEvent { void* equeue; uint64_t id; };
std::vector<GpuEvent> g_gpuEvents;
Clock::time_point g_nextFlip = Clock::now();
std::atomic<int> g_flipRate{0};  // sceVideoOutSetFlipRate: 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz
uint64_t g_flipCount = 0;
gpu::DisplayBufferDesc g_displayBuffers[16];

// With the GPU model active, a flip completes when the command processor
// reaches the frame's flip packet (GfxFlip interrupt), not at submit time.
struct PendingFlip { int buffer; int64_t arg; };
std::mutex g_pendingMutex;
std::deque<PendingFlip> g_pendingFlips;
constexpr uint32_t kIrqGfxFlip = 0x08;

// Waits for this flip's slot. Flips land on vblanks, every (rate + 1) of the
// console's 60 Hz ones. Use the window's real vsync when the rates divide evenly,
// so every frame stays on screen equally long; otherwise a 60 Hz timer.
void WaitFlipSlot() {
    const int vblanksPerFlip = g_flipRate.load() + 1;
    if (video::WaitForFlipSlot(60.0 / vblanksPerFlip)) {
        g_nextFlip = Clock::now();
    } else {
        // Flips stay on a fixed grid of vblanks: the next slot follows from the last slot,
        // never from when a late frame arrived. Restarting the grid at each late frame
        // made the average period drift above the game's fixed step, so every few seconds
        // it ran two steps in one frame and none in the next (a camera snap). The game
        // also paces itself to ~33.3 ms, so frames often arrive just after their slot:
        // up to half a vblank late they flip at once (the grid keeps its beat); later
        // than that they wait for the next vblank on the grid.
        const auto period = kFrameTime * vblanksPerFlip;
        const auto now = Clock::now();
        if (g_nextFlip + period * 4 < now) g_nextFlip = now;  // far behind (a load): resync
        while (g_nextFlip + kFrameTime / 2 < now) g_nextFlip += kFrameTime;
        // Hit the slot precisely, as vsync would: Windows sleeps overshoot by up to a
        // millisecond or so, and the game's fixed 30 Hz simulation then sees uneven frame
        // times and runs 0 or 2 steps in a frame (the camera snap). Sleep most of the way,
        // then spin for the last 2 ms.
        std::this_thread::sleep_until(g_nextFlip - std::chrono::milliseconds(2));
        while (Clock::now() < g_nextFlip) std::this_thread::yield();
        // The slot this frame belongs to, even if it arrived a little late: the main
        // thread's steady clock anchors on the grid, not on when the flip happened.
        kernel::NoteFlip(std::chrono::duration_cast<std::chrono::nanoseconds>(g_nextFlip.time_since_epoch()).count());
        g_nextFlip += period;
        return;
    }
    kernel::NoteFlip(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Reports a completed flip to the game.
void SignalFlip(int64_t flipArg) {
    std::lock_guard lk(g_mutex);
    // Frame-time statistics over each status interval: average, worst, frames over 40 ms.
    static Clock::time_point lastFlip = Clock::now();
    static double sumMs = 0, worstMs = 0;
    static int slow = 0;
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - lastFlip).count();
    lastFlip = now;
    sumMs += ms;
    worstMs = std::max(worstMs, ms);
    slow += ms > 40.0;
    if (++g_flipCount % kFlipsPerStatus == 0) {
        std::fprintf(stderr,
                     "[video] %llu frames flipped (avg %.1f ms, worst %.1f ms, %d over 40 ms; %u new images, "
                     "%u expanded; %u early releases, %u wait addresses learned)\n",
                     static_cast<unsigned long long>(g_flipCount), sumMs / kFlipsPerStatus, worstMs, slow,
                     VideoCore::g_lbp3ImageCreates.exchange(0), VideoCore::g_lbp3ImageExpands.exchange(0),
                     gpu::TakeEarlyReleases(), gpu::LearnedWaits());
        sumMs = worstMs = 0;
        slow = 0;
    }
    for (void* eq : g_flipQueues)
        kernel::TriggerEvent(eq, kInternalFlipEventId, kernel::kEvfiltVideoOut,
                             static_cast<int64_t>(kInternalFlipEventId | (static_cast<uint64_t>(flipArg) << 16)));
}

// Pretends the GPU finished everything submitted so far.
void SignalGpuEvents() {
    std::lock_guard lk(g_mutex);
    for (const auto& e : g_gpuEvents)
        kernel::TriggerEvent(e.equeue, e.id, kernel::kEvfiltGraphicsCore, static_cast<int64_t>(e.id));
}

int PS4_SYSV_ABI sceVideoOutOpen(int32_t userId, int32_t busType, int32_t index, const void* param) {
    (void)userId;
    (void)busType;
    (void)index;
    (void)param;
    video::StartPresenter();
    static const bool gpuReady = [] {
        try {
            if (!gpu::Init()) return false;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[gpu] renderer unavailable (%s); using the scaffolding path\n", e.what());
            return false;
        }
        // GPU interrupts fire the game's sceGnmAddEqEvent registrations with that id.
        gpu::SetInterruptHandler([](uint32_t irq) {
            if (irq == kIrqGfxFlip) {
                PendingFlip f{-1, 0};
                {
                    std::lock_guard pl(g_pendingMutex);
                    if (!g_pendingFlips.empty()) {
                        f = g_pendingFlips.front();
                        g_pendingFlips.pop_front();
                    }
                }
                if (f.buffer >= 0) {
                    video::PresentBuffer(f.buffer);
                    // Pull the rendered image back for our window (on the GPU thread).
                    static std::vector<uint8_t> pixels;  // recycled through the presenter
                    uint32_t w = 0, h = 0;
                    gpu::MaybeDumpRenderTargets();
                    const bool got =
                        f.buffer < 16 && gpu::ReadbackDisplayBuffer(g_displayBuffers[f.buffer], pixels, w, h);
                    // Show the frame on its slot, not whenever the GPU finished it:
                    // uneven hand-over times make a moving camera judder.
                    WaitFlipSlot();
                    if (got) video::PresentPixels(pixels, w, h);
                    SignalFlip(f.arg);
                }
                return;
            }
            std::lock_guard lk(g_mutex);
            static int irqLogged = 0;
            if (irqLogged < 8) {
                ++irqLogged;
                std::fprintf(stderr, "[gnm] interrupt 0x%x (%zu event registrations)\n", irq, g_gpuEvents.size());
            }
            for (const auto& e : g_gpuEvents)
                if (e.id == irq) kernel::TriggerEvent(e.equeue, e.id, kernel::kEvfiltGraphicsCore, static_cast<int64_t>(e.id));
        });
        return true;
    }();
    (void)gpuReady;
    return kHandle;
}

struct BufferAttribute {
    uint32_t pixelFormat, tilingMode;
    int32_t aspectRatio;
    uint32_t width, height, pitchInPixel, option, reserved0;
    uint64_t reserved1;
};

void PS4_SYSV_ABI sceVideoOutSetBufferAttribute(BufferAttribute* attr, uint32_t pixelFormat, uint32_t tilingMode,
                                                uint32_t aspectRatio, uint32_t width, uint32_t height,
                                                uint32_t pitchInPixel) {
    if (!attr) return;
    *attr = {pixelFormat, tilingMode, static_cast<int32_t>(aspectRatio), width, height, pitchInPixel, 0, 0, 0};
}

int PS4_SYSV_ABI sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses, int32_t count,
                                            const BufferAttribute* attr) {
    if (handle != kHandle) return static_cast<int>(0x80290001);
    if (!addresses || !attr) return static_cast<int>(0x80290002);  // INVALID_ADDRESS
    std::fprintf(stderr, "[video] %d display buffers %ux%u (pitch %u, format 0x%x, tiling %u)\n", count, attr->width,
                 attr->height, attr->pitchInPixel, attr->pixelFormat, attr->tilingMode);
    for (int i = 0; i < count; ++i)
    {
        video::SetDisplayBuffer(start + i, {addresses[i], attr->width, attr->height, attr->pitchInPixel,
                                            attr->pixelFormat, attr->tilingMode});
        if (start + i < 16)
            g_displayBuffers[start + i] = {reinterpret_cast<uint64_t>(addresses[i]), attr->width, attr->height,
                                           attr->pitchInPixel, attr->pixelFormat, attr->tilingMode};
    }
    return start;  // the buffer set index
}

int PS4_SYSV_ABI sceVideoOutSetFlipRate(int32_t handle, int32_t rate) {
    if (handle != kHandle) return static_cast<int>(0x80290001);  // INVALID_HANDLE
    if (rate < 0 || rate > 2) return static_cast<int>(0x80290006);  // INVALID_VALUE
    std::fprintf(stderr, "[video] flip rate %d Hz\n", 60 / (rate + 1));
    g_flipRate = rate;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceVideoOutAddFlipEvent(void* equeue, int32_t handle, void* udata) {
    if (handle != kHandle) return static_cast<int>(0x80290001);  // INVALID_HANDLE
    kernel::AddEvent(equeue, kInternalFlipEventId, kernel::kEvfiltVideoOut, udata);
    std::lock_guard lk(g_mutex);
    g_flipQueues.push_back(equeue);
    return kernel::SCE_OK;
}

}  // namespace hle::videoout

namespace hle::gnm {
namespace {

// One 64-bit "flip pending" label per display buffer (sceVideoOutGetBufferLabelAddress).
uint64_t g_bufferLabels[16];

// The game leaves a 64-dword "prepare flip" NOP at the end of the last command
// buffer of a flipped frame; the Gnm driver rewrites it into a label write
// and/or an end-of-pipe event. Ported from shadPS4's PatchFlipRequest.
bool PatchFlipRequest(uint32_t* cmd, uint32_t sizeDw, uint32_t bufferIndex) {
    constexpr uint32_t kPrepareFlipHeader = 0xC03E1000;  // type-3 NOP, 63 body dwords
    enum Payload : uint32_t {
        kPrepareFlip = 0x68750777u, kPrepareFlipLabel = 0x68750778u,
        kPrepareFlipInterrupt = 0x68750780u, kPrepareFlipInterruptLabel = 0x68750781u,
        kPatchedFlip = 0x68750776u,
    };
    constexpr uint32_t kOpNop = 0x10, kOpWriteData = 0x37, kOpEventWriteEop = 0x47;
    auto type3 = [](uint32_t op, uint32_t countMinusOne) { return (3u << 30) | (countMinusOne << 16) | (op << 8); };

    if (sizeDw < 64 || bufferIndex >= std::size(g_bufferLabels)) return false;
    cmd += sizeDw - 64;
    if (cmd[0] != kPrepareFlipHeader) {
        static int warned = 0;
        if (warned++ < 3) std::fprintf(stderr, "[gnm] no prepareFlip packet at the end of the frame\n");
        return false;
    }
    uint32_t b[7];
    std::memcpy(b, cmd, sizeof(b));

    // WRITE_DATA: mark the display buffer as pending (cleared when the flip completes).
    const uint64_t labelAddr = reinterpret_cast<uint64_t>(&g_bufferLabels[bufferIndex]);
    cmd[0] = type3(kOpWriteData, 3);
    cmd[1] = 0x500u;
    cmd[2] = static_cast<uint32_t>(labelAddr);
    cmd[3] = static_cast<uint32_t>(labelAddr >> 32);
    cmd[4] = 1;

    uint32_t* nop = cmd + 5;
    nop[1] = kPatchedFlip;
    if (b[1] == kPrepareFlip) {
        nop[0] = type3(kOpNop, 0x39);
    } else if (b[1] == kPrepareFlipLabel) {
        nop[0] = type3(kOpNop, 0x34);
        uint32_t* w = cmd + 0x3B;  // WRITE_DATA to the game's label
        w[0] = type3(kOpWriteData, 3);
        w[1] = 0x500u;
        w[2] = b[2] & 0xFFFFFFFCu;
        w[3] = b[3];
        w[4] = b[4];
    } else if (b[1] == kPrepareFlipInterruptLabel || b[1] == kPrepareFlipInterrupt) {
        nop[0] = type3(kOpNop, 0x33);
        uint32_t* e = cmd + 0x3A;  // EVENT_WRITE_EOP with interrupt
        e[0] = type3(kOpEventWriteEop, 4);
        e[1] = (b[5] & 0x3F) + 0x500u + (b[6] & 0x3F) * 0x1000;
        if (b[1] == kPrepareFlipInterruptLabel) {
            e[2] = b[2] & 0xFFFFFFFCu;
            e[3] = (b[3] & 0xFFFFu) | 0x22000000u;
            e[4] = b[4];
        } else {
            e[2] = 0;
            e[3] = 0x1000000u;
            e[4] = 0;
        }
        e[5] = 0;
    }
    return true;
}

// Runs the memory-writing packets of each buffer, then raises the GPU
// interrupts the game listens for.
void Process(uint32_t count, void* const* dcbs, const uint32_t* dcbSizes, void* const* ccbs, const uint32_t* ccbSizes) {
    if (gpu::Active()) {
        // shadPS4's command processor executes the buffers and raises the interrupts.
        for (uint32_t i = 0; i < count; ++i) {
            const auto* d = static_cast<const uint32_t*>(dcbs[i]);
            const long bad = pm4::FindOverrun(d, dcbSizes[i] / 4);
            static int reported = 0;
            if (bad >= 0 && reported++ < 3) {
                std::fprintf(stderr, "[gnm] dcb of %u dwords: packet at %ld overruns; tail:", dcbSizes[i] / 4, bad);
                for (uint32_t k = static_cast<uint32_t>(bad); k < dcbSizes[i] / 4; ++k) std::fprintf(stderr, " %08x", d[k]);
                std::fprintf(stderr, "\n");
            }
        }
        for (uint32_t i = 0; i < count; ++i)
            gpu::SubmitGfx(static_cast<const uint32_t*>(dcbs[i]), dcbSizes[i],
                           ccbs ? static_cast<const uint32_t*>(ccbs[i]) : nullptr, ccbSizes ? ccbSizes[i] : 0);
        return;
    }
    bool irq = false;
    static std::atomic<int> submits{0};
    const int n = ++submits;
    for (uint32_t i = 0; i < count; ++i) {
        pm4::Result d{}, c{};
        if (dcbs && dcbs[i]) d = pm4::Execute(static_cast<const uint32_t*>(dcbs[i]), dcbSizes[i]);
        if (ccbs && ccbs[i]) c = pm4::Execute(static_cast<const uint32_t*>(ccbs[i]), ccbSizes[i]);
        irq |= d.interrupt || c.interrupt;
        if (n <= 60 || n % 300 == 0)
            std::fprintf(stderr, "[gnm] submit #%d buf %u: dcb %u bytes, %zu draws, %zu dispatches, target 0x%llx%s\n", n,
                         i, dcbSizes ? dcbSizes[i] : 0, d.draws, d.dispatches,
                         static_cast<unsigned long long>(d.lastColorTarget), d.interrupt ? ", irq" : "");
    }
    if (irq) videoout::SignalGpuEvents();
}

}  // namespace

int PS4_SYSV_ABI sceGnmAddEqEvent(void* equeue, uint64_t id, void* udata) {
    if (!equeue) return kernel::SceErr(kernel::kEBADF);
    kernel::AddEvent(equeue, id, kernel::kEvfiltGraphicsCore, udata);
    std::fprintf(stderr, "[gnm] AddEqEvent id 0x%llx\n", static_cast<unsigned long long>(id));
    std::lock_guard lk(videoout::g_mutex);
    videoout::g_gpuEvents.push_back({equeue, id});
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceGnmDeleteEqEvent(void* equeue, uint64_t id) {
    std::lock_guard lk(videoout::g_mutex);
    std::erase_if(videoout::g_gpuEvents, [&](const auto& e) { return e.equeue == equeue && e.id == id; });
    return kernel::SCE_OK;
}

// The event type lives in the event's data field.
int PS4_SYSV_ABI sceGnmGetEqEventType(const int64_t* event) { return static_cast<int>(event[2]); }

int PS4_SYSV_ABI sceGnmAreSubmitsAllowed() { return 1; }

int PS4_SYSV_ABI sceGnmSubmitCommandBuffers(uint32_t count, void* const* dcbs, const uint32_t* dcbSizes,
                                            void* const* ccbs, const uint32_t* ccbSizes) {
    static int logged = 0;
    if (logged++ < 5)
        std::fprintf(stderr, "[gnm] submit %u command buffer(s), first %u bytes\n", count, count ? dcbSizes[0] : 0);
    Process(count, dcbs, dcbSizes, ccbs, ccbSizes);
    return kernel::SCE_OK;
}

// LBP3_TRACE_SIM=1: how many simulation steps (GetSimFrame__, eboot+0x7c79e0) and game
// frames (GetGameFrame__, 0x7c79f0) each flipped frame contains, 60 flips per line. A
// smooth camera needs exactly one sim step per flip.
void TraceSimSteps() {
    static const bool enabled = std::getenv("LBP3_TRACE_SIM") != nullptr;
    if (!enabled) return;
    uint8_t* base = core::EbootBase();
    if (!base) return;
    using Counter = int(PS4_SYSV_ABI*)();
    const int sim = reinterpret_cast<Counter>(base + 0x7c79e0)();
    const int game = reinterpret_cast<Counter>(base + 0x7c79f0)();
    static int lastSim = sim, lastGame = game;
    static std::string simSteps, gameSteps;
    using SimClock = std::chrono::steady_clock;
    static SimClock::time_point lastTime = SimClock::now();
    static std::string gaps;
    const auto now = SimClock::now();
    const int ds = sim - lastSim, dg = game - lastGame;
    simSteps += ds >= 0 && ds < 10 ? static_cast<char>('0' + ds) : '?';
    gameSteps += dg >= 0 && dg < 10 ? static_cast<char>('0' + dg) : '?';
    const int ms = static_cast<int>(std::chrono::duration<double, std::milli>(now - lastTime).count() + 0.5);
    gaps += std::to_string(ms) + " ";
    lastSim = sim;
    lastGame = game;
    lastTime = now;
    if (simSteps.size() >= 60) {
        std::fprintf(stderr, "[sim] steps per flip: %s | game frames: %s\n[sim] ms between submits: %s\n",
                     simSteps.c_str(), gameSteps.c_str(), gaps.c_str());
        simSteps.clear();
        gameSteps.clear();
        gaps.clear();
    }
}

int PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffers(uint32_t count, void* const* dcbs, const uint32_t* dcbSizes,
                                                   void* const* ccbs, const uint32_t* ccbSizes, int32_t videoOutHandle,
                                                   uint32_t displayBufferIndex, uint32_t flipMode, int64_t flipArg) {
    (void)videoOutHandle;
    static const bool noted = (kernel::NoteMainThread(), true);  // the game's frame thread
    (void)noted;
    TraceSimSteps();
    if (count && gpu::Active()) {
        // Like the real driver: the previous frame must be done before this one is patched.
        gpu::WaitIdle();
        if (PatchFlipRequest(static_cast<uint32_t*>(dcbs[count - 1]), dcbSizes[count - 1] / 4, displayBufferIndex)) {
            {
                std::lock_guard pl(videoout::g_pendingMutex);
                videoout::g_pendingFlips.push_back({static_cast<int>(displayBufferIndex), flipArg});
            }
            Process(count, dcbs, dcbSizes, ccbs, ccbSizes);
            return kernel::SCE_OK;  // flip completes on the GfxFlip interrupt
        }
    }
    (void)displayBufferIndex;
    (void)flipMode;
    static int logged = 0;
    if (logged++ < 5)
        std::fprintf(stderr, "[gnm] submit+flip %u command buffer(s), first %u bytes, buffer %u\n", count,
                     count ? dcbSizes[0] : 0, displayBufferIndex);
    Process(count, dcbs, dcbSizes, ccbs, ccbSizes);
    videoout::WaitFlipSlot();
    video::PresentBuffer(static_cast<int>(displayBufferIndex));
    videoout::SignalFlip(flipArg);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceGnmSubmitDone() { return kernel::SCE_OK; }

// Async compute: the game writes commands into a ring and rings the doorbell.
int PS4_SYSV_ABI sceGnmMapComputeQueue(uint32_t pipeId, uint32_t queueId, void* ringBase, uint32_t ringSizeDw,
                                       uint32_t* readPtr) {
    return gpu::MapComputeQueue(pipeId, queueId, reinterpret_cast<uint64_t>(ringBase), ringSizeDw, readPtr);
}

int PS4_SYSV_ABI sceGnmMapComputeQueueWithPriority(uint32_t pipeId, uint32_t queueId, void* ringBase,
                                                   uint32_t ringSizeDw, uint32_t* readPtr, uint32_t priority) {
    (void)priority;
    return sceGnmMapComputeQueue(pipeId, queueId, ringBase, ringSizeDw, readPtr);
}

int PS4_SYSV_ABI sceGnmUnmapComputeQueue(uint32_t vqid) {
    gpu::UnmapComputeQueue(vqid);
    return kernel::SCE_OK;
}

void PS4_SYSV_ABI sceGnmDingDong(uint32_t vqid, uint32_t nextOffsDw) {
    if (gpu::Active()) gpu::DingDong(vqid, nextOffsDw);
}

void PS4_SYSV_ABI sceGnmDingDongForWorkload(uint32_t vqid, uint32_t nextOffsDw, uint64_t workloadId) {
    (void)workloadId;
    sceGnmDingDong(vqid, nextOffsDw);
}

}  // namespace hle::gnm

namespace hle {

void RegisterVideoOut(core::Linker& linker) {
    using namespace videoout;
    using namespace gnm;
    HLE_REG(linker, "libSceVideoOut", sceVideoOutOpen);
    HLE_REG(linker, "libSceVideoOut", sceVideoOutAddFlipEvent);
    HLE_REG(linker, "libSceVideoOut", sceVideoOutSetFlipRate);
    HLE_REG(linker, "libSceVideoOut", sceVideoOutSetBufferAttribute);
    HLE_REG(linker, "libSceVideoOut", sceVideoOutRegisterBuffers);
    HLE_REG(linker, "libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffers);
    HLE_REG(linker, "libSceGnmDriver", sceGnmSubmitDone);
    HLE_REG(linker, "libSceGnmDriver", sceGnmMapComputeQueue);
    HLE_REG(linker, "libSceGnmDriver", sceGnmMapComputeQueueWithPriority);
    HLE_REG(linker, "libSceGnmDriver", sceGnmUnmapComputeQueue);
    HLE_REG(linker, "libSceGnmDriver", sceGnmDingDong);
    HLE_REG(linker, "libSceGnmDriver", sceGnmDingDongForWorkload);
    HLE_REG(linker, "libSceGnmDriver", sceGnmSubmitCommandBuffers);
    HLE_REG(linker, "libSceGnmDriver", sceGnmAddEqEvent);
    HLE_REG(linker, "libSceGnmDriver", sceGnmDeleteEqEvent);
    HLE_REG(linker, "libSceGnmDriver", sceGnmGetEqEventType);
    HLE_REG(linker, "libSceGnmDriver", sceGnmAreSubmitsAllowed);
}

}  // namespace hle
