// libSceAudioOut on SDL3 audio. Each PS4 port is an SDL audio stream bound to
// the default playback device; SDL converts sample rate and channel count
// (e.g. 7.1 game output on stereo speakers).
//
// sceAudioOutOutput blocks like on the console: the game's mixer (FMOD here)
// hands over one buffer per period and relies on the call to pace it in real
// time. We keep about two periods queued and wait while more than that is
// pending. LBP3_VOLUME=0..100 scales everything.
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::audio {
namespace {

constexpr int kErrorNotOpened = static_cast<int>(0x80260001);
constexpr int kErrorInvalidPort = static_cast<int>(0x80260003);
constexpr int kErrorPortFull = static_cast<int>(0x80260005);
constexpr int kErrorInvalidSize = static_cast<int>(0x80260006);
constexpr int kErrorInvalidFormat = static_cast<int>(0x80260007);
constexpr int kErrorInvalidSampleFreq = static_cast<int>(0x80260008);
constexpr int kErrorNotInit = static_cast<int>(0x8026000F);
constexpr int kErrorSystemResource = static_cast<int>(0x80260011);

constexpr int kMaxPorts = 32;
constexpr float kVolume0dB = 32768.0f;
constexpr int kQueuedPeriods = 3;  // latency target: 3 game buffers (64 ms at LBP3's 1024 frames)

// OrbisAudioOutParamFormat (low byte of the param argument).
struct Format {
    int channels;
    bool isFloat;
    bool std8;  // "standard" 7.1 order (L R C LFE Lb Rb Ls Rs), same as SDL's
};
bool DecodeFormat(uint32_t param, Format& f) {
    switch (param & 0xFF) {
    case 0: f = {1, false, false}; return true;
    case 1: f = {2, false, false}; return true;
    case 2: f = {8, false, false}; return true;
    case 3: f = {1, true, false}; return true;
    case 4: f = {2, true, false}; return true;
    case 5: f = {8, true, false}; return true;
    case 6: f = {8, false, true}; return true;
    case 7: f = {8, true, true}; return true;
    default: return false;
    }
}

struct Port {
    bool open = false;
    int type = 0;
    uint32_t frames = 0, rate = 0;
    Format fmt{};
    SDL_AudioStream* stream = nullptr;
    std::vector<float> scratch;
    float gain = 1.0f;
    uint64_t outputs = 0, underruns = 0;
};

std::mutex g_mutex;
bool g_init = false;
SDL_AudioDeviceID g_device = 0;
Port g_ports[kMaxPorts];

float MasterVolume() {
    static const float v = [] {
        const char* s = std::getenv("LBP3_VOLUME");
        return s ? std::clamp(std::atoi(s), 0, 100) / 100.0f : 1.0f;
    }();
    return v;
}

bool OpenDevice() {
    if (g_device) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!g_device) {
        std::fprintf(stderr, "[audio] no playback device: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{};
    SDL_GetAudioDeviceFormat(g_device, &spec, nullptr);
    std::fprintf(stderr, "[audio] output device: %s (%d Hz, %d ch)\n", SDL_GetAudioDeviceName(g_device), spec.freq,
                 spec.channels);
    return true;
}

Port* GetPort(int handle) {
    if (handle < 1 || handle > kMaxPorts) return nullptr;
    Port* p = &g_ports[handle - 1];
    return p->open ? p : nullptr;
}

// Converts one game buffer to interleaved float in SDL's channel order.
void Convert(const Port& p, const void* src, float* dst) {
    const size_t n = static_cast<size_t>(p.frames) * p.fmt.channels;
    if (p.fmt.isFloat) std::memcpy(dst, src, n * sizeof(float));
    else {
        const auto* s = static_cast<const int16_t*>(src);
        for (size_t i = 0; i < n; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
    }
    if (p.fmt.channels == 8 && !p.fmt.std8) {
        // PS4 order L R C LFE Ls Rs Lb Rb -> SDL 7.1 order FL FR FC LFE BL BR SL SR.
        for (uint32_t f = 0; f < p.frames; ++f) {
            float* fr = dst + f * 8;
            std::swap(fr[4], fr[6]);
            std::swap(fr[5], fr[7]);
        }
    }
}

}  // namespace

int PS4_SYSV_ABI sceAudioOutInit() {
    std::lock_guard lock(g_mutex);
    g_init = true;
    OpenDevice();
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t frames, uint32_t rate,
                                 uint32_t param) {
    (void)userId;
    (void)index;
    std::lock_guard lock(g_mutex);
    if (!g_init) return kErrorNotInit;
    Format fmt;
    if (!DecodeFormat(param, fmt)) return kErrorInvalidFormat;
    if (frames == 0 || frames > 2048 || frames % 256) return kErrorInvalidSize;
    if (rate != 48000) return kErrorInvalidSampleFreq;
    if (!OpenDevice()) return kErrorSystemResource;

    int slot = -1;
    for (int i = 0; i < kMaxPorts; ++i)
        if (!g_ports[i].open) { slot = i; break; }
    if (slot < 0) return kErrorPortFull;

    const SDL_AudioSpec src{SDL_AUDIO_F32, fmt.channels, static_cast<int>(rate)};
    SDL_AudioStream* stream = SDL_CreateAudioStream(&src, nullptr);
    if (!stream || !SDL_BindAudioStream(g_device, stream)) {
        std::fprintf(stderr, "[audio] stream creation failed: %s\n", SDL_GetError());
        if (stream) SDL_DestroyAudioStream(stream);
        return kErrorSystemResource;
    }
    Port& p = g_ports[slot];
    p = Port{};
    p.open = true;
    p.type = type;
    p.frames = frames;
    p.rate = rate;
    p.fmt = fmt;
    p.stream = stream;
    p.scratch.resize(static_cast<size_t>(frames) * fmt.channels);
    SDL_SetAudioStreamGain(stream, MasterVolume());
    std::fprintf(stderr, "[audio] port %d: type %d, %u frames, %d ch %s\n", slot + 1, type, frames, fmt.channels,
                 fmt.isFloat ? "float" : "s16");
    return slot + 1;
}

int PS4_SYSV_ABI sceAudioOutClose(int32_t handle) {
    std::lock_guard lock(g_mutex);
    Port* p = GetPort(handle);
    if (!p) return kErrorInvalidPort;
    SDL_DestroyAudioStream(p->stream);
    *p = Port{};
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceAudioOutOutput(int32_t handle, const void* buffer) {
    SDL_AudioStream* stream;
    size_t bytes;
    {
        std::lock_guard lock(g_mutex);
        Port* p = GetPort(handle);
        if (!p) return kErrorNotOpened;
        stream = p->stream;
        bytes = p->scratch.size() * sizeof(float);
        if (buffer) {
            // An empty queue here (after start-up) means the speaker ran dry: an audible gap.
            if (++p->outputs > 8 && SDL_GetAudioStreamQueued(stream) == 0 && (++p->underruns % 50) == 1)
                std::fprintf(stderr, "[audio] port %d underrun (%llu so far)\n", handle,
                             static_cast<unsigned long long>(p->underruns));
            Convert(*p, buffer, p->scratch.data());
            SDL_PutAudioStreamData(stream, p->scratch.data(), static_cast<int>(bytes));
        }
    }
    // Pace the caller: wait until the queue is back down to the latency target.
    const int target = static_cast<int>(bytes) * kQueuedPeriods;
    for (int i = 0; i < 200 && SDL_GetAudioStreamQueued(stream) > target; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return kernel::SCE_OK;
}

// `flag` has a bit per channel whose volume is given; values are 0..32768 (0 dB).
int PS4_SYSV_ABI sceAudioOutSetVolume(int32_t handle, uint32_t flag, const int32_t* vol) {
    std::lock_guard lock(g_mutex);
    Port* p = GetPort(handle);
    if (!p) return kErrorInvalidPort;
    if (!vol) return kernel::SCE_OK;
    float loudest = 0.0f;
    for (int ch = 0; ch < 8; ++ch)
        if (flag & (1u << ch)) loudest = std::max(loudest, vol[ch] / kVolume0dB);
    p->gain = loudest;
    SDL_SetAudioStreamGain(p->stream, loudest * MasterVolume());
    return kernel::SCE_OK;
}

}  // namespace hle::audio

namespace hle {

void RegisterAudioOut(core::Linker& linker) {
    using namespace audio;
    const char* lib = "libSceAudioOut";
    HLE_REG(linker, lib, sceAudioOutInit);
    HLE_REG(linker, lib, sceAudioOutOpen);
    HLE_REG(linker, lib, sceAudioOutClose);
    HLE_REG(linker, lib, sceAudioOutOutput);
    HLE_REG(linker, lib, sceAudioOutSetVolume);
}

}  // namespace hle
