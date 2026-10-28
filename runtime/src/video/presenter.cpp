#include "video/presenter.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace video {
namespace {

constexpr int kMaxBuffers = 16;
constexpr int kWindowWidth = 1280, kWindowHeight = 720;

std::mutex g_mutex;
DisplayBuffer g_buffers[kMaxBuffers];
std::atomic<int> g_shown{-1};
std::atomic<bool> g_started{false};

std::vector<uint8_t> g_frame;  // latest GPU-rendered frame, guarded by g_mutex
uint32_t g_frameW = 0, g_frameH = 0;
bool g_haveFrame = false;
uint64_t g_frameSeq = 0;  // bumps with every new frame, so unchanged ones aren't re-uploaded

// Real display refreshes: the present loop blocks on vsync, so each pass is one.
std::mutex g_vblankMutex;
std::condition_variable g_vblankCv;
uint64_t g_vblanks = 0;
std::atomic<float> g_refreshHz{0.0f};
std::atomic<void (*)(int)> g_hotkeyHandler{nullptr};

void PresenterThread() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[video] SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    SDL_Window* window = SDL_CreateWindow("LittleBigPlanet 3 (native)", kWindowWidth, kWindowHeight,
                                          SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "[video] window creation failed: %s\n", SDL_GetError());
        return;
    }
    const bool vsyncOk = SDL_SetRenderVSync(renderer, 1);
    int vsync = 0;
    SDL_GetRenderVSync(renderer, &vsync);
    std::fprintf(stderr, "[video] window renderer: %s, vsync %s (%d)\n", SDL_GetRendererName(renderer),
                 vsyncOk ? "on" : "FAILED", vsync);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
        g_refreshHz = mode->refresh_rate;
        std::fprintf(stderr, "[video] display refresh %.2f Hz\n", mode->refresh_rate);
    }
    SDL_Texture* texture = nullptr;
    uint32_t texW = 0, texH = 0;
    uint64_t shownSeq = ~0ull;
    std::vector<uint8_t> frame;

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (auto handler = g_hotkeyHandler.load()) handler(static_cast<int>(e.key.key));
            }
            if (e.type == SDL_EVENT_QUIT) {
                std::fprintf(stderr, "[video] window closed\n");
                std::fflush(stderr);
                ExitProcess(0);
            }
        }

        DisplayBuffer buf;
        bool fresh = true;
        {
            std::lock_guard lock(g_mutex);
            if (g_haveFrame) {
                fresh = g_frameSeq != shownSeq;
                if (fresh) {
                    frame.swap(g_frame);  // take it; our previous buffer goes back to the GPU thread
                    shownSeq = g_frameSeq;
                }
                buf = {frame.data(), g_frameW, g_frameH, g_frameW, 0, 0};
            } else {
                const int i = g_shown.load();
                if (i >= 0 && i < kMaxBuffers) buf = g_buffers[i];
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (buf.address && buf.width && buf.height) {
            if (!texture || texW != buf.width || texH != buf.height) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                            static_cast<int>(buf.width), static_cast<int>(buf.height));
                texW = buf.width;
                texH = buf.height;
            }
            // 32-bit BGRA rows. Only upload when a new frame arrived.
            if (fresh || !g_haveFrame)
                SDL_UpdateTexture(texture, nullptr, buf.address, static_cast<int>(buf.pitchPixels * 4));
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer);  // paced by vsync
        {
            std::lock_guard lock(g_vblankMutex);
            ++g_vblanks;
        }
        g_vblankCv.notify_all();
    }
}

}  // namespace

bool WaitForFlipSlot(double gameHz) {
    const float refresh = g_refreshHz.load();
    if (refresh <= 0.0f || gameHz <= 0.0) return false;
    const double ratio = refresh / gameHz;
    const uint64_t perFlip = static_cast<uint64_t>(ratio + 0.5);
    if (perFlip == 0 || std::abs(ratio - static_cast<double>(perFlip)) > 0.05) return false;  // e.g. 30 fps on 144 Hz
    static uint64_t next = 0;
    std::unique_lock lock(g_vblankMutex);
    if (next == 0 || next > g_vblanks + perFlip) next = g_vblanks + perFlip;  // first flip / after a resync
    const bool ok = g_vblankCv.wait_for(lock, std::chrono::milliseconds(200), [&] { return g_vblanks >= next; });
    if (!ok) return false;  // window not presenting (minimized?): use the timer
    // Running late keeps the cadence from the current vblank instead of trying to catch up.
    next = std::max(next, g_vblanks) + perFlip;
    return true;
}

void StartPresenter() {
    if (g_started.exchange(true)) return;
    std::thread(PresenterThread).detach();
}

void SetDisplayBuffer(int index, const DisplayBuffer& buffer) {
    if (index < 0 || index >= kMaxBuffers) return;
    std::lock_guard lock(g_mutex);
    g_buffers[index] = buffer;
}

void SetHotkeyHandler(void (*handler)(int key)) { g_hotkeyHandler = handler; }

void TriggerHotkey(int key) {
    if (auto handler = g_hotkeyHandler.load()) handler(key);
}

void PresentBuffer(int index) { g_shown = index; }

// LBP3_DUMP_FRAMES=skip:count writes `count` frames after the first `skip` (quarter size, 24-bit BMP) to
// analysis/frames/ for looking at animation and flicker frame by frame.
static std::atomic<int> g_frameDumpRequest{0};

void RequestFrameDump(int count) { g_frameDumpRequest = count; }

static void MaybeDumpFrame(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    static int skip = 0, limit = 0;
    static const bool parsed = [] {
        if (const char* v = std::getenv("LBP3_DUMP_FRAMES")) std::sscanf(v, "%d:%d", &skip, &limit);
        return true;
    }();
    (void)parsed;
    static int seen = 0, n = 0;
    if (const int more = g_frameDumpRequest.exchange(0); more > 0) {  // RequestFrameDump
        skip = seen;
        limit = n + more;
    }
    if (seen++ < skip || n >= limit || w < 4 || h < 4) return;
    if (FILE* t = std::fopen("analysis\\frames\\times.txt", n == 0 ? "w" : "a")) {  // hand-over times
        std::fprintf(t, "%d %.3f\n", n,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
                         .count());
        std::fclose(t);
    }
    const uint32_t ow = w / 4, oh = h / 4, row = (ow * 3 + 3) & ~3u;
    std::vector<uint8_t> out(54 + row * oh);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    out[0] = 'B'; out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size())); put32(10, 54); put32(14, 40);
    put32(18, ow); put32(22, oh); out[26] = 1; out[28] = 24; put32(34, row * oh);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const uint8_t* s = &px[((h - 1 - y * 4) * w + x * 4) * 4];  // BMP rows go bottom-up
            uint8_t* d = &out[54 + y * row + x * 3];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    CreateDirectoryA("analysis\\frames", nullptr);
    char name[64];
    std::snprintf(name, sizeof(name), "analysis\\frames\\f%04d.bmp", n++);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

void PresentPixels(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    MaybeDumpFrame(pixels, width, height);
    {
        // Frame delivery stats: uneven gaps between frames look like judder/tearing.
        using Clock = std::chrono::steady_clock;
        static Clock::time_point last = Clock::now();
        static double minMs = 1e9, maxMs = 0, sumMs = 0;
        static int n = 0;
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        minMs = std::min(minMs, ms);
        maxMs = std::max(maxMs, ms);
        sumMs += ms;
        if (++n == 300) {
            std::fprintf(stderr, "[video] frame gaps over 300 frames: min %.1f ms, avg %.1f ms, max %.1f ms\n", minMs,
                         sumMs / n, maxMs);
            minMs = 1e9, maxMs = 0, sumMs = 0, n = 0;
        }
    }
    std::lock_guard lock(g_mutex);
    g_frame.swap(pixels);  // the caller gets an older buffer back to refill: no allocation per frame
    g_frameW = width;
    g_frameH = height;
    g_haveFrame = true;
    ++g_frameSeq;
}

}  // namespace video
