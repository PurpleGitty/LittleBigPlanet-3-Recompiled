// libkernel threading: threads, attributes, keys, once, mutexes, condition
// variables and read/write locks, on top of Win32 threads and SRW locks.
//
// Every Sce handle here (ScePthread, ScePthreadMutex, ...) is a pointer-sized
// value. 0 means "statically initialized", so synchronisation objects are
// created lazily on first use.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/runner.h"
#include "core/tls.h"
#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::kernel {
namespace {

using Clock = std::chrono::steady_clock;
using PthreadEntry = void*(PS4_SYSV_ABI*)(void*);
using KeyDestructor = void(PS4_SYSV_ABI*)(void*);

constexpr size_t kDefaultStackSize = 2u << 20;
// Our own code (logging, stubs, Windows APIs, exception dispatch) also runs on
// game threads and needs more stack than the PS4 system libraries did. It's
// only reserved address space, so be generous.
constexpr size_t kMinStackSize = 1u << 20;
// Kept free for exception handlers, so a stack overflow gets reported instead
// of the process vanishing.
constexpr ULONG kStackGuarantee = 64u << 10;
constexpr int kDefaultPriority = 700;  // PS4: 256 (highest) .. 767 (lowest)
constexpr uint64_t kAllCores = 0x7F;   // 7 cores available to games
constexpr int kMaxKeys = 256;
constexpr int kKeyDestructorPasses = 4;

// --- threads ----------------------------------------------------------------

struct PthreadAttr {
    size_t stackSize = kDefaultStackSize;
    void* stackAddr = nullptr;
    bool detached = false;
    int priority = kDefaultPriority;
    int policy = 1;   // SCHED_FIFO
    int inherit = 4;  // PTHREAD_INHERIT_SCHED
    uint64_t affinity = kAllCores;
};

struct Pthread {
    HANDLE handle = nullptr;
    DWORD tid = 0;
    PthreadEntry entry = nullptr;
    void* arg = nullptr;
    void* ret = nullptr;
    std::string name;
    std::atomic<bool> detached{false};
    PthreadAttr attr;
    void* specific[kMaxKeys] = {};
};

thread_local Pthread* t_self = nullptr;

struct Key { std::atomic<bool> used{false}; KeyDestructor dtor = nullptr; };
Key g_keys[kMaxKeys];

Pthread* Self() {
    if (!t_self) {
        // A thread we didn't create (the main thread): adopt it.
        auto* t = new Pthread;
        t->tid = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        t->name = "main";
        t_self = t;
    }
    return t_self;
}

void RunKeyDestructors(Pthread* t) {
    for (int pass = 0; pass < kKeyDestructorPasses; ++pass) {
        bool any = false;
        for (int k = 0; k < kMaxKeys; ++k) {
            void* v = t->specific[k];
            if (!v || !g_keys[k].used.load() || !g_keys[k].dtor) continue;
            t->specific[k] = nullptr;
            g_keys[k].dtor(v);
            any = true;
        }
        if (!any) break;
    }
}

void FinishThread(Pthread* t) {
    RunKeyDestructors(t);
    core::UnregisterCurrentThread();
    core::FreeThreadTls();
}

}  // namespace

// Commits the rest of this thread's stack, as on the console, where a thread's whole stack
// is usable from the start. Game code doesn't probe its stack, so a big local buffer could
// skip over the guard page or hit it while fiber code has the stack bounds swapped (a
// guard page violation in memcpy). Two pages at the bottom stay reserved to catch overflow.
void CommitWholeStack() {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(&mbi, &mbi, sizeof(mbi))) return;
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    char* const low = static_cast<char*>(mbi.AllocationBase) + 2 * 4096;
    char* const limit = static_cast<char*>(tib->StackLimit);
    if (limit <= low) return;
    if (VirtualAlloc(low, static_cast<size_t>(limit - low), MEM_COMMIT, PAGE_READWRITE))
        tib->StackLimit = low;
}

namespace {

DWORD WINAPI ThreadProc(void* p) {
    auto* t = static_cast<Pthread*>(p);
    t_self = t;
    // Name it first, so thread dumps show it even if setup below blocks.
    std::wstring wname(t->name.begin(), t->name.end());
    SetThreadDescription(GetCurrentThread(), wname.c_str());
    CommitWholeStack();
    ULONG guarantee = kStackGuarantee;
    SetThreadStackGuarantee(&guarantee);
    core::SetupThreadTls();
    core::RegisterCurrentThread(t->name);
    t->ret = t->entry(t->arg);
    std::fprintf(stderr, "[thread] %s exited (returned %p)\n", t->name.c_str(), t->ret);
    FinishThread(t);
    return 0;
}

PthreadAttr* AttrOf(const void* const* attr) {
    return attr && *attr ? static_cast<PthreadAttr*>(const_cast<void*>(*attr)) : nullptr;
}

// --- mutexes ----------------------------------------------------------------

enum MutexType : int { kErrorCheck = 1, kRecursive = 2, kNormal = 3, kAdaptive = 4 };

struct MutexAttr { int type = kErrorCheck; int protocol = 0; };

struct Mutex {
    SRWLOCK lock = SRWLOCK_INIT;
    int type = kErrorCheck;
    std::atomic<DWORD> owner{0};
    int depth = 0;          // recursion count while owned
    void* lockSite = nullptr;  // game code that last took it, for diagnostics
};

template <typename T>
T* Lazy(void* handle) {
    auto* slot = reinterpret_cast<std::atomic<T*>*>(handle);
    T* obj = slot->load(std::memory_order_acquire);
    if (obj) return obj;
    auto* fresh = new T;
    if (slot->compare_exchange_strong(obj, fresh)) return fresh;
    delete fresh;  // another thread got there first
    return obj;
}

void TakeOwnership(Mutex* m, int depth) {
    m->owner.store(GetCurrentThreadId(), std::memory_order_relaxed);
    m->depth = depth;
}

// Game code address that called into us (for "who locked this" reports).
#define GAME_CALLER() __builtin_return_address(0)

int LockMutex(Mutex* m, const Clock::time_point* deadline, void* site) {
    const DWORD me = GetCurrentThreadId();
    if (m->owner.load(std::memory_order_relaxed) == me) {
        if (m->type == kRecursive) { ++m->depth; return SCE_OK; }
        // Like FreeBSD: a timed self-lock waits out the timeout, an untimed one fails.
        if (deadline) {
            std::this_thread::sleep_until(*deadline);
            return SceErr(kETIMEDOUT);
        }
        if (m->type != kNormal) return SceErr(kEDEADLK);
    }
    if (!deadline) {
        AcquireSRWLockExclusive(&m->lock);
    } else {
        // SRW locks have no timed acquire.
        while (!TryAcquireSRWLockExclusive(&m->lock)) {
            if (Clock::now() >= *deadline) {
                static std::atomic<int> reported{0};
                if (reported++ < 5)
                    std::fprintf(stderr, "[kernel] timed lock at %s gave up; held by '%s' since %s (depth %d)\n",
                                 core::DescribeAddress(reinterpret_cast<uint64_t>(site)).c_str(),
                                 core::ThreadName(m->owner.load()).c_str(),
                                 core::DescribeAddress(reinterpret_cast<uint64_t>(m->lockSite)).c_str(), m->depth);
                return SceErr(kETIMEDOUT);
            }
            SwitchToThread();
        }
    }
    TakeOwnership(m, 1);
    m->lockSite = site;
    return SCE_OK;
}

// --- condition variables / rwlocks ------------------------------------------

struct Cond { CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT; };

struct Rwlock {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> writer{0};
};

int CondWait(void* cond, void* mutex, const DWORD timeoutMs) {
    if (!cond || !mutex) return SceErr(kEINVAL);
    Cond* c = Lazy<Cond>(cond);
    Mutex* m = Lazy<Mutex>(mutex);
    if (m->owner.load(std::memory_order_relaxed) != GetCurrentThreadId()) return SceErr(kEPERM);
    const int depth = m->depth;
    m->owner.store(0, std::memory_order_relaxed);
    BOOL ok = SleepConditionVariableSRW(&c->cv, &m->lock, timeoutMs, 0);
    TakeOwnership(m, depth);
    return ok ? SCE_OK : SceErr(kETIMEDOUT);
}

DWORD UsecToMs(uint32_t usec) { return (usec + 999) / 1000; }

}  // namespace

// --- thread attributes --------------------------------------------------------

int PS4_SYSV_ABI scePthreadAttrInit(void** attr) {
    if (!attr) return SceErr(kEINVAL);
    *attr = new PthreadAttr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrDestroy(void** attr) {
    if (!attr || !*attr) return SceErr(kEINVAL);
    delete static_cast<PthreadAttr*>(*attr);
    *attr = nullptr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetstacksize(void** attr, size_t size) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    a->stackSize = size;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetstack(void** attr, void* addr, size_t size) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    // Windows threads can't run on a caller-provided stack; we allocate our own
    // of the same size instead.
    a->stackAddr = addr;
    a->stackSize = size;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetdetachstate(void** attr, int state) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    a->detached = state == 1;  // PTHREAD_CREATE_DETACHED
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetinheritsched(void** attr, int inherit) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    a->inherit = inherit;
    return SCE_OK;
}

struct SchedParam { int priority; };

int PS4_SYSV_ABI scePthreadAttrSetschedparam(void** attr, const SchedParam* param) {
    auto* a = AttrOf(attr);
    if (!a || !param) return SceErr(kEINVAL);
    a->priority = param->priority;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetschedpolicy(void** attr, int policy) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    a->policy = policy;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrSetaffinity(void** attr, uint64_t mask) {
    auto* a = AttrOf(attr);
    if (!a) return SceErr(kEINVAL);
    a->affinity = mask;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrGetaffinity(void* const* attr, uint64_t* mask) {
    auto* a = AttrOf(attr);
    if (!a || !mask) return SceErr(kEINVAL);
    *mask = a->affinity;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadAttrGet(Pthread* thread, void** attr) {
    auto* a = AttrOf(attr);
    if (!thread || !a) return SceErr(kEINVAL);
    *a = thread->attr;
    a->detached = thread->detached.load();
    return SCE_OK;
}

// --- threads ------------------------------------------------------------------

int PS4_SYSV_ABI scePthreadCreate(Pthread** thread, void* const* attr, PthreadEntry entry, void* arg,
                                  const char* name) {
    if (!thread || !entry) return SceErr(kEINVAL);
    auto* t = new (std::nothrow) Pthread;
    if (!t) return SceErr(kENOMEM);
    if (auto* a = AttrOf(attr)) t->attr = *a;
    t->entry = entry;
    t->arg = arg;
    t->name = name ? name : "thread";
    t->detached = t->attr.detached;

    const size_t stack = std::max(t->attr.stackSize, kMinStackSize);
    t->handle = CreateThread(nullptr, stack, ThreadProc, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION,
                             &t->tid);
    if (!t->handle) {
        delete t;
        return SceErr(kEAGAIN);
    }
    *thread = t;  // visible before the thread can call scePthreadSelf()
    std::fprintf(stderr, "[thread] %s (prio %d, stack %zu KB)\n", t->name.c_str(), t->attr.priority, stack / 1024);
    ResumeThread(t->handle);
    return SCE_OK;
}

Pthread* PS4_SYSV_ABI scePthreadSelf() { return Self(); }

int PS4_SYSV_ABI scePthreadJoin(Pthread* thread, void** value) {
    if (!thread) return SceErr(kESRCH);
    if (thread == Self()) return SceErr(kEDEADLK);
    if (thread->detached) return SceErr(kEINVAL);
    WaitForSingleObject(thread->handle, INFINITE);
    if (value) *value = thread->ret;
    CloseHandle(thread->handle);
    delete thread;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadDetach(Pthread* thread) {
    if (!thread) return SceErr(kESRCH);
    thread->detached = true;
    return SCE_OK;
}

[[noreturn]] void PS4_SYSV_ABI scePthreadExit(void* value) {
    Pthread* t = Self();
    t->ret = value;
    std::fprintf(stderr, "[thread] %s called pthread_exit (%p)\n", t->name.c_str(), value);
    FinishThread(t);
    ExitThread(0);
}

int PS4_SYSV_ABI scePthreadEqual(Pthread* a, Pthread* b) { return a == b; }

void PS4_SYSV_ABI scePthreadYield() { SwitchToThread(); }
int PS4_SYSV_ABI sched_yield() { SwitchToThread(); return 0; }

int PS4_SYSV_ABI scePthreadGetprio(Pthread* thread, int* prio) {
    if (!thread || !prio) return SceErr(kEINVAL);
    *prio = thread->attr.priority;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadSetprio(Pthread* thread, int prio) {
    if (!thread) return SceErr(kESRCH);
    thread->attr.priority = prio;  // TODO: map to Windows priorities if timing needs it
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadGetschedparam(Pthread* thread, int* policy, SchedParam* param) {
    if (!thread) return SceErr(kESRCH);
    if (policy) *policy = thread->attr.policy;
    if (param) param->priority = thread->attr.priority;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadSetschedparam(Pthread* thread, int policy, const SchedParam* param) {
    if (!thread) return SceErr(kESRCH);
    thread->attr.policy = policy;
    if (param) thread->attr.priority = param->priority;
    return SCE_OK;
}

int PS4_SYSV_ABI pthread_setschedparam(Pthread* thread, int policy, const SchedParam* param) {
    return ToErrno(scePthreadSetschedparam(thread, policy, param));  // pthread_* return errno directly
}

int PS4_SYSV_ABI scePthreadSetaffinity(Pthread* thread, uint64_t mask) {
    if (!thread) return SceErr(kESRCH);
    thread->attr.affinity = mask;  // not applied: let Windows schedule freely
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadGetaffinity(Pthread* thread, uint64_t* mask) {
    if (!thread || !mask) return SceErr(kEINVAL);
    *mask = thread->attr.affinity;
    return SCE_OK;
}

// --- once / keys ----------------------------------------------------------------

struct PthreadOnce { std::atomic<int> state; void* mutex; };

int PS4_SYSV_ABI scePthreadOnce(PthreadOnce* once, void(PS4_SYSV_ABI* init)()) {
    if (!once || !init) return SceErr(kEINVAL);
    enum { kNotDone = 0, kRunning = 1, kDone = 2 };
    int expected = kNotDone;
    if (once->state.compare_exchange_strong(expected, kRunning)) {
        init();
        once->state.store(kDone, std::memory_order_release);
        return SCE_OK;
    }
    while (once->state.load(std::memory_order_acquire) != kDone) SwitchToThread();
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadKeyCreate(int* key, KeyDestructor dtor) {
    if (!key) return SceErr(kEINVAL);
    for (int k = 0; k < kMaxKeys; ++k) {
        bool expected = false;
        if (g_keys[k].used.compare_exchange_strong(expected, true)) {
            g_keys[k].dtor = dtor;
            *key = k;
            return SCE_OK;
        }
    }
    return SceErr(kEAGAIN);
}

int PS4_SYSV_ABI scePthreadKeyDelete(int key) {
    if (key < 0 || key >= kMaxKeys || !g_keys[key].used) return SceErr(kEINVAL);
    g_keys[key].dtor = nullptr;
    g_keys[key].used = false;
    return SCE_OK;
}

void* PS4_SYSV_ABI scePthreadGetspecific(int key) {
    if (key < 0 || key >= kMaxKeys) return nullptr;
    return Self()->specific[key];
}

int PS4_SYSV_ABI scePthreadSetspecific(int key, const void* value) {
    if (key < 0 || key >= kMaxKeys || !g_keys[key].used) return SceErr(kEINVAL);
    Self()->specific[key] = const_cast<void*>(value);
    return SCE_OK;
}

// --- mutexes --------------------------------------------------------------------

int PS4_SYSV_ABI scePthreadMutexattrInit(void** attr) {
    if (!attr) return SceErr(kEINVAL);
    *attr = new MutexAttr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexattrDestroy(void** attr) {
    if (!attr || !*attr) return SceErr(kEINVAL);
    delete static_cast<MutexAttr*>(*attr);
    *attr = nullptr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexattrSettype(void** attr, int type) {
    if (!attr || !*attr || type < kErrorCheck || type > kAdaptive) return SceErr(kEINVAL);
    static_cast<MutexAttr*>(*attr)->type = type;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexattrSetprotocol(void** attr, int protocol) {
    if (!attr || !*attr) return SceErr(kEINVAL);
    static_cast<MutexAttr*>(*attr)->protocol = protocol;  // priority protocols are ignored
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexInit(void** mutex, void* const* attr, const char*) {
    if (!mutex) return SceErr(kEINVAL);
    auto* m = new (std::nothrow) Mutex;
    if (!m) return SceErr(kENOMEM);
    if (attr && *attr) m->type = static_cast<const MutexAttr*>(*attr)->type;
    *mutex = m;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexDestroy(void** mutex) {
    if (!mutex) return SceErr(kEINVAL);
    auto* m = static_cast<Mutex*>(*mutex);
    if (m && m->owner.load()) return SceErr(kEBUSY);
    delete m;
    *mutex = nullptr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexLock(void** mutex) {
    if (!mutex) return SceErr(kEINVAL);
    return LockMutex(Lazy<Mutex>(mutex), nullptr, GAME_CALLER());
}

int PS4_SYSV_ABI scePthreadMutexTimedlock(void** mutex, uint32_t usec) {
    if (!mutex) return SceErr(kEINVAL);
    {
        // Sample the caller of hot timed locks (busy polling loops show up here).
        static std::atomic<uint64_t> calls{0};
        const uint64_t n = ++calls;
        if (n % 100000 == 0)
            std::fprintf(stderr, "[kernel] timed lock #%llu from %s (%u us) on '%s'\n",
                         static_cast<unsigned long long>(n),
                         core::DescribeAddress(reinterpret_cast<uint64_t>(GAME_CALLER())).c_str(), usec,
                         core::ThreadName(GetCurrentThreadId()).c_str());
    }
    const auto deadline = Clock::now() + std::chrono::microseconds(usec);
    return LockMutex(Lazy<Mutex>(mutex), &deadline, GAME_CALLER());
}

int PS4_SYSV_ABI scePthreadMutexTrylock(void** mutex) {
    if (!mutex) return SceErr(kEINVAL);
    Mutex* m = Lazy<Mutex>(mutex);
    if (m->owner.load(std::memory_order_relaxed) == GetCurrentThreadId() && m->type == kRecursive) {
        ++m->depth;
        return SCE_OK;
    }
    if (!TryAcquireSRWLockExclusive(&m->lock)) return SceErr(kEBUSY);
    TakeOwnership(m, 1);
    m->lockSite = GAME_CALLER();
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadMutexUnlock(void** mutex) {
    if (!mutex) return SceErr(kEINVAL);
    Mutex* m = Lazy<Mutex>(mutex);
    if (m->owner.load(std::memory_order_relaxed) != GetCurrentThreadId()) {
        static std::atomic<int> reported{0};
        if (reported++ < 5)
            std::fprintf(stderr, "[kernel] unlock at %s by '%s' refused: held by '%s' since %s\n",
                         core::DescribeAddress(reinterpret_cast<uint64_t>(GAME_CALLER())).c_str(),
                         core::ThreadName(GetCurrentThreadId()).c_str(), core::ThreadName(m->owner.load()).c_str(),
                         core::DescribeAddress(reinterpret_cast<uint64_t>(m->lockSite)).c_str());
        return SceErr(kEPERM);
    }
    if (--m->depth > 0) return SCE_OK;
    m->owner.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&m->lock);
    return SCE_OK;
}

// --- condition variables --------------------------------------------------------

int PS4_SYSV_ABI scePthreadCondInit(void** cond, const void*, const char*) {
    if (!cond) return SceErr(kEINVAL);
    *cond = new Cond;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadCondDestroy(void** cond) {
    if (!cond) return SceErr(kEINVAL);
    delete static_cast<Cond*>(*cond);
    *cond = nullptr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadCondSignal(void** cond) {
    if (!cond) return SceErr(kEINVAL);
    WakeConditionVariable(&Lazy<Cond>(cond)->cv);
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadCondBroadcast(void** cond) {
    if (!cond) return SceErr(kEINVAL);
    WakeAllConditionVariable(&Lazy<Cond>(cond)->cv);
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadCondWait(void** cond, void** mutex) { return CondWait(cond, mutex, INFINITE); }

int PS4_SYSV_ABI scePthreadCondTimedwait(void** cond, void** mutex, uint32_t usec) {
    return CondWait(cond, mutex, UsecToMs(usec));
}

// --- read/write locks -----------------------------------------------------------

int PS4_SYSV_ABI scePthreadRwlockInit(void** lock, const void*, const char*) {
    if (!lock) return SceErr(kEINVAL);
    *lock = new Rwlock;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadRwlockDestroy(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    delete static_cast<Rwlock*>(*lock);
    *lock = nullptr;
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadRwlockRdlock(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    AcquireSRWLockShared(&Lazy<Rwlock>(lock)->lock);
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadRwlockWrlock(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    Rwlock* l = Lazy<Rwlock>(lock);
    AcquireSRWLockExclusive(&l->lock);
    l->writer = GetCurrentThreadId();
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadRwlockTryrdlock(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    return TryAcquireSRWLockShared(&Lazy<Rwlock>(lock)->lock) ? SCE_OK : SceErr(kEBUSY);
}

int PS4_SYSV_ABI scePthreadRwlockTrywrlock(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    Rwlock* l = Lazy<Rwlock>(lock);
    if (!TryAcquireSRWLockExclusive(&l->lock)) return SceErr(kEBUSY);
    l->writer = GetCurrentThreadId();
    return SCE_OK;
}

int PS4_SYSV_ABI scePthreadRwlockUnlock(void** lock) {
    if (!lock) return SceErr(kEINVAL);
    Rwlock* l = Lazy<Rwlock>(lock);
    if (l->writer.load() == GetCurrentThreadId()) {
        l->writer = 0;
        ReleaseSRWLockExclusive(&l->lock);
    } else {
        ReleaseSRWLockShared(&l->lock);
    }
    return SCE_OK;
}

}  // namespace hle::kernel

namespace hle {

void RegisterKernelPthread(core::Linker& linker) {
    using namespace kernel;
    const char* lib = "libkernel";
    HLE_REG(linker, lib, scePthreadAttrInit);
    HLE_REG(linker, lib, scePthreadAttrDestroy);
    HLE_REG(linker, lib, scePthreadAttrSetstacksize);
    HLE_REG(linker, lib, scePthreadAttrSetstack);
    HLE_REG(linker, lib, scePthreadAttrSetdetachstate);
    HLE_REG(linker, lib, scePthreadAttrSetinheritsched);
    HLE_REG(linker, lib, scePthreadAttrSetschedparam);
    HLE_REG(linker, lib, scePthreadAttrSetschedpolicy);
    HLE_REG(linker, lib, scePthreadAttrSetaffinity);
    HLE_REG(linker, lib, scePthreadAttrGetaffinity);
    HLE_REG(linker, lib, scePthreadAttrGet);
    HLE_REG(linker, lib, scePthreadCreate);
    HLE_REG(linker, lib, scePthreadSelf);
    HLE_REG(linker, lib, scePthreadJoin);
    HLE_REG(linker, lib, scePthreadDetach);
    HLE_REG(linker, lib, scePthreadExit);
    HLE_REG(linker, lib, scePthreadEqual);
    HLE_REG(linker, lib, scePthreadYield);
    HLE_REG(linker, lib, sched_yield);
    HLE_REG(linker, lib, scePthreadGetprio);
    HLE_REG(linker, lib, scePthreadSetprio);
    HLE_REG(linker, lib, scePthreadGetschedparam);
    HLE_REG(linker, lib, scePthreadSetschedparam);
    HLE_REG(linker, lib, scePthreadSetaffinity);
    HLE_REG(linker, lib, scePthreadGetaffinity);
    HLE_REG(linker, lib, scePthreadOnce);
    HLE_REG(linker, lib, scePthreadKeyCreate);
    HLE_REG(linker, lib, scePthreadKeyDelete);
    HLE_REG(linker, lib, scePthreadGetspecific);
    HLE_REG(linker, lib, scePthreadSetspecific);
    HLE_REG(linker, lib, scePthreadMutexattrInit);
    HLE_REG(linker, lib, scePthreadMutexattrDestroy);
    HLE_REG(linker, lib, scePthreadMutexattrSettype);
    HLE_REG(linker, lib, scePthreadMutexattrSetprotocol);
    HLE_REG(linker, lib, scePthreadMutexInit);
    HLE_REG(linker, lib, scePthreadMutexDestroy);
    HLE_REG(linker, lib, scePthreadMutexLock);
    HLE_REG(linker, lib, scePthreadMutexTimedlock);
    HLE_REG(linker, lib, scePthreadMutexTrylock);
    HLE_REG(linker, lib, scePthreadMutexUnlock);
    HLE_REG(linker, lib, scePthreadCondInit);
    HLE_REG(linker, lib, scePthreadCondDestroy);
    HLE_REG(linker, lib, scePthreadCondSignal);
    HLE_REG(linker, lib, scePthreadCondBroadcast);
    HLE_REG(linker, lib, scePthreadCondWait);
    HLE_REG(linker, lib, scePthreadCondTimedwait);
    HLE_REG(linker, lib, scePthreadRwlockInit);
    HLE_REG(linker, lib, scePthreadRwlockDestroy);
    HLE_REG(linker, lib, scePthreadRwlockRdlock);
    HLE_REG(linker, lib, scePthreadRwlockWrlock);
    HLE_REG(linker, lib, scePthreadRwlockTryrdlock);
    HLE_REG(linker, lib, scePthreadRwlockTrywrlock);
    HLE_REG(linker, lib, scePthreadRwlockUnlock);
    HLE_REG(linker, "libScePosix", pthread_setschedparam);
}

}  // namespace hle
