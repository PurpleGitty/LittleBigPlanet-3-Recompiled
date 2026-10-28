// libkernel time: clocks, sleeping, the TSC and process info.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#include "core/runner.h"
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::kernel {
namespace {

struct Timespec { int64_t sec, nsec; };
struct Timeval { int64_t sec, usec; };
struct Timezone { int minutesWest, dstTime; };

enum ClockId : int {
    kClockRealtime = 0,
    kClockVirtual = 1,
    kClockProf = 2,
    kClockMonotonic = 4,
    kClockUptime = 5,
    kClockUptimePrecise = 7,
    kClockUptimeFast = 8,
    kClockRealtimePrecise = 9,
    kClockRealtimeFast = 10,
    kClockMonotonicPrecise = 11,
    kClockMonotonicFast = 12,
    kClockSecond = 13,
    kClockThreadCputime = 14,
    kClockProcTime = 15,
};

constexpr int64_t kFileTimeToUnixEpoch = 116444736000000000LL;  // 100 ns units
constexpr uint32_t kPageSize = 16 * 1024;

const int64_t g_qpcFreq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    timeBeginPeriod(1);  // 1 ms sleep granularity, like a console
    return f.QuadPart;
}();
const int64_t g_qpcStart = [] {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}();

int64_t QpcNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int64_t WallNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - kFileTimeToUnixEpoch) * 100;
}

int64_t QpcToNs(int64_t t) { return t / g_qpcFreq * 1'000'000'000 + t % g_qpcFreq * 1'000'000'000 / g_qpcFreq; }

// Frame-steady time for the game's main thread (LBP3_FRAME_CLOCK=off disables). LBP3
// runs a fixed 30 Hz simulation off the monotonic clock it reads at the start of each
// frame. On the console frames start exactly on vsync; here the main thread is released
// a variable moment after the flip, so the readings were 32-36 ms apart and the
// simulation ran 0 or 2 steps in some frames (camera snaps). On the main thread, reads
// within kDeadZone of the last flip return the flip's time (the flips are precise);
// later reads run at the real rate, kDeadZone behind, until the next flip. Never goes
// backwards, and stays exact over time. Other threads see real time.
std::atomic<int64_t> g_lastFlipNs{0};  // steady_clock (QPC-based) time of the last flip
std::atomic<DWORD> g_mainThread{0};
constexpr int64_t kDeadZoneNs = 8'000'000;

bool FrameClockEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("LBP3_FRAME_CLOCK");
        return !v || std::string(v) != "off";
    }();
    return on;
}

// The threads that step the simulation: the main thread, and the session's "game host
// thread", which sends one simulation tick per frame (if it paced off real time, its
// ticks drifted against the flips and frames got 0 or 2 ticks).
bool OnFrameClock() {
    if (GetCurrentThreadId() == g_mainThread.load(std::memory_order_relaxed)) return true;
    thread_local int isHost = -1;
    if (isHost < 0) isHost = core::ThreadName(GetCurrentThreadId()) == "game host thread" ? 1 : 0;
    return isHost == 1;
}

int64_t MonotonicNs() {
    const int64_t real = QpcToNs(QpcNow());
    const int64_t flipNs = g_lastFlipNs.load(std::memory_order_relaxed);
    if (!flipNs || !FrameClockEnabled() || !OnFrameClock()) return real;
    const int64_t nowSteady = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t elapsed = nowSteady - flipNs;  // time since the last flip
    const int64_t steady = elapsed < kDeadZoneNs ? real - elapsed : real - kDeadZoneNs;
    thread_local int64_t lastReturned = 0;
    lastReturned = std::max(lastReturned, steady);
    return lastReturned;
}

int64_t CpuNs(HANDLE h, bool thread) {
    FILETIME c, e, k, u;
    BOOL ok = thread ? GetThreadTimes(h, &c, &e, &k, &u) : GetProcessTimes(h, &c, &e, &k, &u);
    if (!ok) return 0;
    auto v = [](FILETIME f) { return static_cast<int64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime; };
    return (v(k) + v(u)) * 100;
}

void SleepUs(uint64_t usec) {
    std::this_thread::sleep_for(std::chrono::microseconds(usec));
}

// LBP3_TRACE_TIME=1: every 5 s, log which game code reads which clock, and how often
// (to find the frame-timing code).
// `frame` is the HLE function's frame: it holds the game caller's saved rbp, and the
// game keeps frame-pointer chains, so the next two callers can be read from it.
void NoteTimeCall(const char* fn, void* caller, void* frame) {
    static const bool enabled = std::getenv("LBP3_TRACE_TIME") != nullptr;
    if (!enabled) return;
    using Key = std::tuple<const char*, uint64_t, uint64_t, uint64_t>;
    static std::mutex mutex;
    static std::map<Key, uint64_t> counts;
    static int64_t lastReport = QpcNow();
    // Only follow frame pointers that stay on this stack (the crash handler sees any fault
    // before an SEH guard would).
    uint64_t up[2]{};
    const auto low = reinterpret_cast<uint64_t>(frame);
    auto onStack = [&](const uint64_t* p) {
        const auto v = reinterpret_cast<uint64_t>(p);
        return v % 8 == 0 && v > low && v < low + (4u << 20);
    };
    const auto* rbp = *static_cast<uint64_t* const*>(frame);  // the game caller's frame
    for (auto& r : up) {
        if (!onStack(rbp)) break;
        r = rbp[1];
        rbp = reinterpret_cast<const uint64_t*>(rbp[0]);
    }
    std::lock_guard lock(mutex);
    ++counts[{fn, reinterpret_cast<uint64_t>(caller), up[0], up[1]}];
    if (QpcNow() - lastReport < 5 * g_qpcFreq) return;
    lastReport = QpcNow();
    std::vector<std::pair<uint64_t, Key>> sorted;
    for (const auto& [key, n] : counts) sorted.push_back({n, key});
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(stderr, "[time] clock reads in the last 5 s:\n");
    for (size_t i = 0; i < sorted.size() && i < 25; ++i) {
        const auto& [name, c0, c1, c2] = sorted[i].second;
        std::fprintf(stderr, "[time]   %6llu  %s from %s < %s < %s\n",
                     static_cast<unsigned long long>(sorted[i].first), name, core::DescribeAddress(c0).c_str(),
                     core::DescribeAddress(c1).c_str(), core::DescribeAddress(c2).c_str());
    }
    counts.clear();
}
#define NOTE_TIME_CALL() NoteTimeCall(__func__, __builtin_return_address(0), __builtin_frame_address(0))

}  // namespace

void NoteFlip(int64_t steadyNs) { g_lastFlipNs.store(steadyNs, std::memory_order_relaxed); }
void NoteMainThread() { g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed); }

int PS4_SYSV_ABI sceKernelClockGettime(int clock, Timespec* tp) {
    NOTE_TIME_CALL();
    if (!tp) return SceErr(kEFAULT);
    int64_t ns;
    switch (clock) {
    case kClockRealtime:
    case kClockRealtimePrecise:
    case kClockRealtimeFast:
        ns = WallNs();
        break;
    case kClockSecond:
        ns = WallNs() / 1'000'000'000 * 1'000'000'000;
        break;
    case kClockMonotonic:
    case kClockMonotonicPrecise:
    case kClockMonotonicFast:
    case kClockUptime:
    case kClockUptimePrecise:
    case kClockUptimeFast:
        ns = MonotonicNs();
        break;
    case kClockThreadCputime:
        ns = CpuNs(GetCurrentThread(), true);
        break;
    case kClockVirtual:
    case kClockProf:
    case kClockProcTime:
        ns = CpuNs(GetCurrentProcess(), false);
        break;
    default:
        return SceErr(kEINVAL);
    }
    tp->sec = ns / 1'000'000'000;
    tp->nsec = ns % 1'000'000'000;
    return SCE_OK;
}

int PS4_SYSV_ABI clock_gettime(int clock, Timespec* tp) { return PosixResult(sceKernelClockGettime(clock, tp)); }

int PS4_SYSV_ABI sceKernelGettimeofday(Timeval* tv) {
    NOTE_TIME_CALL();
    if (!tv) return SceErr(kEFAULT);
    const int64_t us = WallNs() / 1000;
    tv->sec = us / 1'000'000;
    tv->usec = us % 1'000'000;
    return SCE_OK;
}

int PS4_SYSV_ABI gettimeofday(Timeval* tv, Timezone* tz) {
    if (tz) *tz = {};
    return PosixResult(tv ? sceKernelGettimeofday(tv) : SCE_OK);
}

int PS4_SYSV_ABI sceKernelGettimezone(Timezone* tz) {
    if (!tz) return SceErr(kEFAULT);
    TIME_ZONE_INFORMATION info;
    DWORD r = GetTimeZoneInformation(&info);
    tz->minutesWest = static_cast<int>(info.Bias);
    tz->dstTime = r == TIME_ZONE_ID_DAYLIGHT;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelUsleep(uint32_t usec) {
    NOTE_TIME_CALL();
    SleepUs(usec);
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelSleep(uint32_t sec) {
    SleepUs(uint64_t{sec} * 1'000'000);
    return SCE_OK;
}

int PS4_SYSV_ABI nanosleep(const Timespec* req, Timespec* rem) {
    NOTE_TIME_CALL();
    if (!req || req->nsec < 0 || req->nsec >= 1'000'000'000) return PosixResult(SceErr(kEINVAL));
    SleepUs(static_cast<uint64_t>(req->sec) * 1'000'000 + static_cast<uint64_t>(req->nsec) / 1000);
    if (rem) *rem = {};
    return 0;
}

// Microseconds since the process started.
uint64_t PS4_SYSV_ABI sceKernelGetProcessTime() {
    NOTE_TIME_CALL();
    const int64_t t = QpcNow() - g_qpcStart;
    return static_cast<uint64_t>(t / g_qpcFreq * 1'000'000 + t % g_qpcFreq * 1'000'000 / g_qpcFreq);
}

// The "TSC" is the performance counter; games only compare it against the frequency below.
uint64_t PS4_SYSV_ABI sceKernelReadTsc() {
    NOTE_TIME_CALL();
    return static_cast<uint64_t>(QpcNow());
}
uint64_t PS4_SYSV_ABI sceKernelGetTscFrequency() { return static_cast<uint64_t>(g_qpcFreq); }

int PS4_SYSV_ABI getpid() { return static_cast<int>(GetCurrentProcessId()); }
int PS4_SYSV_ABI getpagesize() { return kPageSize; }

}  // namespace hle::kernel

namespace hle {

void RegisterKernelTime(core::Linker& linker) {
    using namespace kernel;
    const char* lib = "libkernel";
    HLE_REG(linker, lib, sceKernelClockGettime);
    HLE_REG(linker, lib, clock_gettime);
    HLE_REG(linker, "libScePosix", clock_gettime);
    HLE_REG(linker, lib, sceKernelGettimeofday);
    HLE_REG(linker, lib, gettimeofday);
    HLE_REG(linker, lib, sceKernelGettimezone);
    HLE_REG(linker, lib, sceKernelUsleep);
    HLE_REG(linker, lib, sceKernelSleep);
    HLE_REG(linker, lib, nanosleep);
    HLE_REG(linker, lib, sceKernelGetProcessTime);
    HLE_REG(linker, lib, sceKernelReadTsc);
    HLE_REG(linker, lib, sceKernelGetTscFrequency);
    HLE_REG(linker, lib, getpid);
    HLE_REG(linker, lib, getpagesize);
}

}  // namespace hle
