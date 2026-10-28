// libkernel synchronisation: semaphores (sce and POSIX), event flags and
// event queues (kqueue-style; timers, user events and other libraries' events).
#include "hle/kernel_sync.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::kernel {
namespace {

using Clock = std::chrono::steady_clock;

// Waits on `cv` until `ready` or the in/out microsecond timeout runs out.
// A null timeout waits forever. Returns false on timeout (and zeroes it).
template <typename Pred>
bool WaitWithTimeout(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint32_t* timeout, Pred ready) {
    if (!timeout) {
        cv.wait(lk, ready);
        return true;
    }
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::microseconds(*timeout);
    if (!cv.wait_until(lk, deadline, ready)) {
        *timeout = 0;
        return false;
    }
    auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
    *timeout = left > 0 ? static_cast<uint32_t>(left) : 0;
    return true;
}

// --- semaphores ---------------------------------------------------------------

struct Sema {
    std::mutex m;
    std::condition_variable cv;
    std::string name;
    int count = 0, initial = 0, max = 0;
    int waiters = 0;
    uint32_t cancelGeneration = 0;
};

int SemaWait(Sema* s, int need, uint32_t* timeout) {
    if (!s || need <= 0 || need > s->max) return SceErr(kEINVAL);
    std::unique_lock lk(s->m);
    const uint32_t gen = s->cancelGeneration;
    ++s->waiters;
    bool ok = WaitWithTimeout(s->cv, lk, timeout, [&] { return s->count >= need || s->cancelGeneration != gen; });
    --s->waiters;
    if (!ok) return SceErr(kETIMEDOUT);
    if (s->cancelGeneration != gen) return SceErr(kECANCELED);
    s->count -= need;
    return SCE_OK;
}

int SemaSignal(Sema* s, int n) {
    if (!s) return SceErr(kEINVAL);
    std::lock_guard lk(s->m);
    if (s->count + n > s->max) return SceErr(kEINVAL);
    s->count += n;
    s->cv.notify_all();
    return SCE_OK;
}

// --- event flags ------------------------------------------------------------------

struct EventFlag {
    std::mutex m;
    std::condition_variable cv;
    std::string name;
    uint64_t bits = 0;
};

enum EventFlagWait : uint32_t { kWaitAnd = 0x01, kWaitOr = 0x02, kClearAll = 0x10, kClearPattern = 0x20 };

// --- event queues -------------------------------------------------------------------

struct Event {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    void* udata;
};
static_assert(sizeof(Event) == 32, "SceKernelEvent layout mismatch");

struct Registration {
    uint64_t ident;
    int16_t filter;
    void* udata;
    HANDLE timer = nullptr;
};

struct Equeue {
    std::mutex m;
    std::condition_variable cv;
    std::string name;
    std::vector<Registration> regs;
    std::deque<Event> pending;
};

struct TimerContext { Equeue* eq; uint64_t ident; };

void Trigger(Equeue* eq, uint64_t ident, int16_t filter, int64_t data, bool accumulate) {
    std::lock_guard lk(eq->m);
    const Registration* reg = nullptr;
    for (const auto& r : eq->regs)
        if (r.ident == ident && r.filter == filter) reg = &r;
    if (!reg) return;
    // Repeated triggers before the game collects the event merge into one.
    for (auto& e : eq->pending) {
        if (e.ident == ident && e.filter == filter) {
            e.data = accumulate ? e.data + data : data;
            return;
        }
    }
    eq->pending.push_back({ident, filter, 0, 0, data, reg->udata});
    eq->cv.notify_all();
}

VOID CALLBACK TimerFired(PVOID p, BOOLEAN) {
    auto* ctx = static_cast<TimerContext*>(p);
    Trigger(ctx->eq, ctx->ident, kEvfiltTimer, 1, true);  // data = expirations
}

void RemoveRegistration(Equeue* eq, uint64_t ident, int16_t filter) {
    std::lock_guard lk(eq->m);
    for (auto it = eq->regs.begin(); it != eq->regs.end(); ++it) {
        if (it->ident != ident || it->filter != filter) continue;
        if (it->timer) DeleteTimerQueueTimer(nullptr, it->timer, nullptr);  // TimerContext leaks; tiny
        eq->regs.erase(it);
        break;
    }
}

}  // namespace

void AddEvent(void* equeue, uint64_t ident, int16_t filter, void* udata) {
    auto* eq = static_cast<Equeue*>(equeue);
    if (!eq) return;
    std::lock_guard lk(eq->m);
    for (auto& r : eq->regs) {
        if (r.ident == ident && r.filter == filter) { r.udata = udata; return; }
    }
    eq->regs.push_back({ident, filter, udata});
    std::fprintf(stderr, "[kernel] equeue '%s': listening for ident 0x%llx filter %d\n", eq->name.c_str(),
                 static_cast<unsigned long long>(ident), filter);
}

void TriggerEvent(void* equeue, uint64_t ident, int16_t filter, int64_t data) {
    if (equeue) Trigger(static_cast<Equeue*>(equeue), ident, filter, data, false);
}

// --- sce semaphores ---------------------------------------------------------------

int PS4_SYSV_ABI sceKernelCreateSema(Sema** sem, const char* name, uint32_t attr, int initial, int max,
                                     const void* opt) {
    (void)attr;
    (void)opt;
    if (!sem || initial < 0 || max <= 0 || initial > max) return SceErr(kEINVAL);
    auto* s = new Sema;
    s->name = name ? name : "";
    s->count = s->initial = initial;
    s->max = max;
    *sem = s;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelDeleteSema(Sema* sem) {
    if (!sem) return SceErr(kESRCH);
    {
        std::lock_guard lk(sem->m);
        if (sem->waiters) return SceErr(kEBUSY);
    }
    delete sem;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelWaitSema(Sema* sem, int need, uint32_t* timeout) { return SemaWait(sem, need, timeout); }

int PS4_SYSV_ABI sceKernelSignalSema(Sema* sem, int count) { return SemaSignal(sem, count); }

int PS4_SYSV_ABI sceKernelPollSema(Sema* sem, int need) {
    if (!sem || need <= 0) return SceErr(kEINVAL);
    std::lock_guard lk(sem->m);
    if (sem->count < need) return SceErr(kEBUSY);
    sem->count -= need;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelCancelSema(Sema* sem, int setCount, int* numWaiters) {
    if (!sem) return SceErr(kESRCH);
    std::lock_guard lk(sem->m);
    if (numWaiters) *numWaiters = sem->waiters;
    sem->count = setCount < 0 ? sem->initial : setCount;
    ++sem->cancelGeneration;
    sem->cv.notify_all();
    return SCE_OK;
}

// --- POSIX semaphores ---------------------------------------------------------------

struct Timespec { int64_t sec, nsec; };

int PS4_SYSV_ABI sem_init(Sema** sem, int pshared, unsigned value) {
    (void)pshared;
    if (!sem) return PosixResult(SceErr(kEINVAL));
    auto* s = new Sema;
    s->count = s->initial = static_cast<int>(value);
    s->max = 0x7FFFFFFF;
    *sem = s;
    return 0;
}

int PS4_SYSV_ABI sem_destroy(Sema** sem) {
    if (!sem || !*sem) return PosixResult(SceErr(kEINVAL));
    delete *sem;
    *sem = nullptr;
    return 0;
}

int PS4_SYSV_ABI sem_wait(Sema** sem) { return PosixResult(sem ? SemaWait(*sem, 1, nullptr) : SceErr(kEINVAL)); }

int PS4_SYSV_ABI sem_post(Sema** sem) { return PosixResult(sem ? SemaSignal(*sem, 1) : SceErr(kEINVAL)); }

int PS4_SYSV_ABI sem_timedwait(Sema** sem, const Timespec* abstime) {
    if (!sem || !abstime) return PosixResult(SceErr(kEINVAL));
    // abstime is wall-clock time.
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t nowUs = ((static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL) / 10;
    const int64_t thenUs = abstime->sec * 1000000 + abstime->nsec / 1000;
    uint32_t timeout = thenUs > nowUs ? static_cast<uint32_t>(std::min<int64_t>(thenUs - nowUs, 0xFFFFFFFF)) : 0;
    return PosixResult(SemaWait(*sem, 1, &timeout));
}

// --- event flags ------------------------------------------------------------------------

int PS4_SYSV_ABI sceKernelCreateEventFlag(EventFlag** ef, const char* name, uint32_t attr, uint64_t initial,
                                          const void* opt) {
    (void)attr;
    (void)opt;
    if (!ef) return SceErr(kEINVAL);
    auto* f = new EventFlag;
    f->name = name ? name : "";
    f->bits = initial;
    *ef = f;
    std::fprintf(stderr, "[kernel] event flag '%s' created (%p)\n", f->name.c_str(), static_cast<void*>(f));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEventFlag(EventFlag* ef) {
    if (!ef) return SceErr(kESRCH);
    delete ef;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelSetEventFlag(EventFlag* ef, uint64_t bits) {
    if (!ef) return SceErr(kESRCH);
    std::lock_guard lk(ef->m);
    ef->bits |= bits;
    ef->cv.notify_all();
    static const bool trace = std::getenv("LBP3_TRACE_EF") != nullptr;
    static std::atomic<int> n{0};
    if (trace && n++ < 400)
        std::fprintf(stderr, "[ef] set '%s' |= %llx -> %llx\n", ef->name.c_str(), static_cast<unsigned long long>(bits),
                     static_cast<unsigned long long>(ef->bits));
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelClearEventFlag(EventFlag* ef, uint64_t bits) {
    if (!ef) return SceErr(kESRCH);
    std::lock_guard lk(ef->m);
    ef->bits &= bits;  // PS4 semantics: AND with the given mask
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelWaitEventFlag(EventFlag* ef, uint64_t pattern, uint32_t mode, uint64_t* result,
                                        uint32_t* timeout) {
    if (!ef || !pattern) return SceErr(kEINVAL);
    {
        static const bool trace = std::getenv("LBP3_TRACE_EF") != nullptr;
        static std::atomic<int> n{0};
        if (trace && n++ < 60)
            std::fprintf(stderr, "[ef] wait '%s' pattern %llx mode %x timeout %s (bits now %llx)\n",
                         ef->name.c_str(), static_cast<unsigned long long>(pattern), mode,
                         timeout ? std::to_string(*timeout).c_str() : "none",
                         static_cast<unsigned long long>(ef->bits));
    }
    std::unique_lock lk(ef->m);
    auto ready = [&] { return (mode & kWaitAnd) ? (ef->bits & pattern) == pattern : (ef->bits & pattern) != 0; };
    bool ok = WaitWithTimeout(ef->cv, lk, timeout, ready);
    if (result) *result = ef->bits;
    if (!ok) return SceErr(kETIMEDOUT);
    if (mode & kClearAll) ef->bits = 0;
    else if (mode & kClearPattern) ef->bits &= ~pattern;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelPollEventFlag(EventFlag* ef, uint64_t pattern, uint32_t mode, uint64_t* result) {
    uint32_t zero = 0;
    int r = sceKernelWaitEventFlag(ef, pattern, mode, result, &zero);
    return r == SceErr(kETIMEDOUT) ? SceErr(kEBUSY) : r;
}

// --- event queues ------------------------------------------------------------------------

int PS4_SYSV_ABI sceKernelCreateEqueue(Equeue** eq, const char* name) {
    if (!eq) return SceErr(kEINVAL);
    auto* q = new Equeue;
    q->name = name ? name : "";
    *eq = q;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(Equeue* eq) {
    if (!eq) return SceErr(kEBADF);
    for (const auto& r : eq->regs)
        if (r.timer) DeleteTimerQueueTimer(nullptr, r.timer, INVALID_HANDLE_VALUE);
    delete eq;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelWaitEqueue(Equeue* eq, Event* events, int num, int* out, uint32_t* timeout) {
    if (!eq || !events || num < 1) return SceErr(kEINVAL);
    std::unique_lock lk(eq->m);
    bool ok = WaitWithTimeout(eq->cv, lk, timeout, [&] { return !eq->pending.empty(); });
    int n = 0;
    while (n < num && !eq->pending.empty()) {
        events[n++] = eq->pending.front();
        eq->pending.pop_front();
    }
    if (out) *out = n;
    return ok ? SCE_OK : SceErr(kETIMEDOUT);
}

int PS4_SYSV_ABI sceKernelAddUserEvent(Equeue* eq, int id) {
    if (!eq) return SceErr(kEBADF);
    AddEvent(eq, static_cast<uint64_t>(id), kEvfiltUser, nullptr);
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(Equeue* eq, int id, void* udata) {
    if (!eq) return SceErr(kEBADF);
    {
        std::lock_guard lk(eq->m);
        for (auto& r : eq->regs)
            if (r.ident == static_cast<uint64_t>(id) && r.filter == kEvfiltUser) r.udata = udata;
    }
    Trigger(eq, static_cast<uint64_t>(id), kEvfiltUser, 0, false);
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(Equeue* eq, int id, uint32_t usec, void* udata) {
    if (!eq) return SceErr(kEBADF);
    RemoveRegistration(eq, static_cast<uint64_t>(id), kEvfiltTimer);
    AddEvent(eq, static_cast<uint64_t>(id), kEvfiltTimer, udata);
    const DWORD ms = std::max<DWORD>(1, usec / 1000);
    auto* ctx = new TimerContext{eq, static_cast<uint64_t>(id)};
    HANDLE timer = nullptr;
    if (!CreateTimerQueueTimer(&timer, nullptr, TimerFired, ctx, ms, ms, WT_EXECUTEDEFAULT)) {
        delete ctx;
        return SceErr(kENOMEM);
    }
    std::lock_guard lk(eq->m);
    for (auto& r : eq->regs)
        if (r.ident == static_cast<uint64_t>(id) && r.filter == kEvfiltTimer) r.timer = timer;
    return SCE_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(Equeue* eq, int id) {
    if (!eq) return SceErr(kEBADF);
    RemoveRegistration(eq, static_cast<uint64_t>(id), kEvfiltTimer);
    return SCE_OK;
}

int16_t PS4_SYSV_ABI sceKernelGetEventFilter(const Event* ev) { return ev->filter; }
uint64_t PS4_SYSV_ABI sceKernelGetEventId(const Event* ev) { return ev->ident; }
int64_t PS4_SYSV_ABI sceKernelGetEventData(const Event* ev) { return ev->data; }
void* PS4_SYSV_ABI sceKernelGetEventUserData(const Event* ev) { return ev->udata; }

}  // namespace hle::kernel

namespace hle {

void RegisterKernelSync(core::Linker& linker) {
    using namespace kernel;
    const char* lib = "libkernel";
    HLE_REG(linker, lib, sceKernelCreateSema);
    HLE_REG(linker, lib, sceKernelDeleteSema);
    HLE_REG(linker, lib, sceKernelWaitSema);
    HLE_REG(linker, lib, sceKernelSignalSema);
    HLE_REG(linker, lib, sceKernelPollSema);
    HLE_REG(linker, lib, sceKernelCancelSema);
    HLE_REG(linker, lib, sceKernelCreateEventFlag);
    HLE_REG(linker, lib, sceKernelDeleteEventFlag);
    HLE_REG(linker, lib, sceKernelSetEventFlag);
    HLE_REG(linker, lib, sceKernelClearEventFlag);
    HLE_REG(linker, lib, sceKernelWaitEventFlag);
    HLE_REG(linker, lib, sceKernelPollEventFlag);
    HLE_REG(linker, lib, sceKernelCreateEqueue);
    HLE_REG(linker, lib, sceKernelDeleteEqueue);
    HLE_REG(linker, lib, sceKernelWaitEqueue);
    HLE_REG(linker, lib, sceKernelAddUserEvent);
    HLE_REG(linker, lib, sceKernelTriggerUserEvent);
    HLE_REG(linker, lib, sceKernelAddTimerEvent);
    HLE_REG(linker, lib, sceKernelDeleteTimerEvent);
    HLE_REG(linker, lib, sceKernelGetEventFilter);
    HLE_REG(linker, lib, sceKernelGetEventId);
    HLE_REG(linker, lib, sceKernelGetEventData);
    HLE_REG(linker, lib, sceKernelGetEventUserData);
    const char* posix = "libScePosix";
    HLE_REG(linker, posix, sem_init);
    HLE_REG(linker, posix, sem_destroy);
    HLE_REG(linker, posix, sem_wait);
    HLE_REG(linker, posix, sem_post);
    HLE_REG(linker, posix, sem_timedwait);
}

}  // namespace hle
