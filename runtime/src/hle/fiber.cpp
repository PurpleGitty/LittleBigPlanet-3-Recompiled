// libSceFiber: cooperative fibers on game-provided stacks. Adapted from shadPS4
// (src/core/libraries/fiber, GPL-2.0-or-later): same context layout, setjmp /
// longjmp assembly and state machine.
//
// LBP3 runs jobs as fibers and waits for each one's sceFiberRun to report 4
// ("done"); with the library stubbed the Pod's loading jobs never finished.
//
// Windows addition: while a fiber runs on its own stack, the TEB's stack bounds
// are moved to that stack, so stack probing (__chkstk) and exception dispatch
// in the host code it calls see a consistent stack.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/tls.h"
#include "hle/register.h"

namespace hle::fiber {
namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kErrorNull = static_cast<int32_t>(0x80590001);
constexpr int32_t kErrorAlignment = static_cast<int32_t>(0x80590002);
constexpr int32_t kErrorRange = static_cast<int32_t>(0x80590003);
constexpr int32_t kErrorInvalid = static_cast<int32_t>(0x80590004);
constexpr int32_t kErrorPermission = static_cast<int32_t>(0x80590005);
constexpr int32_t kErrorState = static_cast<int32_t>(0x80590006);

constexpr uint32_t kSignature0 = 0xdef1649c;
constexpr uint32_t kSignature1 = 0xb37592a0;
constexpr uint32_t kOptSignature = 0xbb40e64d;
constexpr uint64_t kStackSignature = 0x7149f2ca7149f2ca;
constexpr uint64_t kStackSizeCheck = 0xdeadbeefdeadbeef;
constexpr uint64_t kMaxNameLength = 31;
constexpr uint64_t kContextMinimumSize = 512;
constexpr uint32_t kFw350 = 0x3500000;

enum FiberState : uint32_t { kRun = 1, kIdle = 2, kTerminated = 3 };
enum FiberFlags : uint32_t { kContextSizeCheck = 0x10, kSetFpuRegs = 0x100 };

using Entry = void PS4_SYSV_ABI (*)(uint64_t argOnInitialize, uint64_t argOnRun);

struct Fiber;

struct Context {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, r8, r9, r10, r11, r12, r13, r14, r15;  // offsets 0x00..0x68
    uint16_t fpucw;                                                                  // 0x70
    uint32_t mxcsr;                                                                  // 0x72 (packed)
    Fiber* currentFiber;
    Fiber* prevFiber;
    uint64_t argOnRunTo;
    uint64_t argOnReturn;
    uint64_t returnVal;
    void* threadStackBase;   // the thread's own TEB stack bounds, restored when a
    void* threadStackLimit;  // fiber gives control back to it
};

struct EntryData {
    Entry entry;               // 0x00
    uint64_t argOnInitialize;  // 0x08
    uint64_t argOnRunTo;       // 0x10
    void* stackAddr;           // 0x18
    uint32_t* state;           // 0x20
    uint16_t fpucw;            // 0x28
    int8_t pad[2];
    uint32_t mxcsr;            // 0x2c
};

// Layout shared with the game (it allocates these).
struct Fiber {
    uint32_t magicStart;
    std::atomic<uint32_t> state;
    Entry entry;
    uint64_t argOnInitialize;
    void* addrContext;
    uint64_t sizeContext;
    char name[kMaxNameLength + 1];
    Context* context;
    uint32_t flags;
    void* contextStart;
    void* contextEnd;
    uint32_t magicEnd;
};
static_assert(sizeof(Fiber) <= 256);

struct FiberInfo {
    uint64_t size;
    Entry entry;
    uint64_t argOnInitialize;
    void* addrContext;
    uint64_t sizeContext;
    char name[kMaxNameLength + 1];
    uint64_t sizeContextMargin;
    uint8_t pad[48];
};
static_assert(sizeof(FiberInfo) == 128);

struct OptParam {
    uint32_t magic;
};

std::atomic<uint32_t> g_contextSizeCheck{0};

}  // namespace

extern "C" {
int32_t PS4_SYSV_ABI lbp3_FiberSetJmp(Context* ctx);
void PS4_SYSV_ABI lbp3_FiberLongJmp(Context* ctx);
void PS4_SYSV_ABI lbp3_FiberSwitchEntry(EntryData* data, bool setFpu);
void PS4_SYSV_ABI lbp3_FiberForceQuit(uint64_t ret);
}

asm(R"(
    .att_syntax prefix
    .text
    .globl lbp3_FiberSetJmp
lbp3_FiberSetJmp:
    movq %rax, 0x00(%rdi)
    movq (%rsp), %rdx
    movq %rdx, 0x10(%rdi)
    movq %rcx, 0x08(%rdi)
    movq %rbx, 0x18(%rdi)
    movq %rsp, 0x20(%rdi)
    movq %rbp, 0x28(%rdi)
    movq %r8,  0x30(%rdi)
    movq %r9,  0x38(%rdi)
    movq %r10, 0x40(%rdi)
    movq %r11, 0x48(%rdi)
    movq %r12, 0x50(%rdi)
    movq %r13, 0x58(%rdi)
    movq %r14, 0x60(%rdi)
    movq %r15, 0x68(%rdi)
    fnstcw  0x70(%rdi)
    stmxcsr 0x72(%rdi)
    xorl %eax, %eax
    ret

    .globl lbp3_FiberLongJmp
lbp3_FiberLongJmp:
    stmxcsr -0x4(%rsp)
    movl 0x72(%rdi), %eax
    andl $0xffffffc0, %eax
    movl -0x4(%rsp), %ecx
    andl $0x3f, %ecx
    xorl %eax, %ecx
    movl %ecx, -0x4(%rsp)
    ldmxcsr -0x4(%rsp)
    movq 0x00(%rdi), %rax
    movq 0x08(%rdi), %rcx
    movq 0x10(%rdi), %rdx
    movq 0x18(%rdi), %rbx
    movq 0x20(%rdi), %rsp
    movq 0x28(%rdi), %rbp
    movq 0x30(%rdi), %r8
    movq 0x38(%rdi), %r9
    movq 0x40(%rdi), %r10
    movq 0x48(%rdi), %r11
    movq 0x50(%rdi), %r12
    movq 0x58(%rdi), %r13
    movq 0x60(%rdi), %r14
    movq 0x68(%rdi), %r15
    fldcw 0x70(%rdi)
    movq %rdx, 0x00(%rsp)
    movl $0x1, %eax
    ret

    .globl lbp3_FiberSwitchEntry
lbp3_FiberSwitchEntry:
    mov %rdi, %r11
    movq 0x18(%r11), %rsp
    xorl %ebp, %ebp
    movq 0x20(%r11), %r10
    test %r10, %r10
    jz 1f
    movl $2, (%r10)
1:
    test %esi, %esi
    jz 2f
    ldmxcsr 0x2c(%r11)
    fldcw 0x28(%r11)
2:
    movq 0x08(%r11), %rdi
    movq 0x10(%r11), %rsi
    movq 0x00(%r11), %r11
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    vzeroall
    call *%r11
    movl $1, %edi
    call lbp3_FiberForceQuit
    int3
    .intel_syntax noprefix
)");

namespace {

Context* CurrentContext() { return static_cast<Context*>(core::CurrentTcb()->fiber); }

void SetStackBounds(void* base, void* limit) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->StackBase = base;
    tib->StackLimit = limit;
}

// The stack `fiber` runs on: its own context area, or the thread's stack.
void EnterStackOf(const Fiber* fiber, const Context* ctx) {
    if (fiber && fiber->addrContext)
        SetStackBounds(static_cast<uint8_t*>(fiber->addrContext) + fiber->sizeContext, fiber->addrContext);
    else
        SetStackBounds(ctx->threadStackBase, ctx->threadStackLimit);
}

[[noreturn]] void Fatal(const char* what) {
    std::fprintf(stderr, "[fiber] fatal: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

void CheckStackOverflow(const Context* ctx) {
    const auto* base = static_cast<const uint64_t*>(ctx->currentFiber->addrContext);
    if (base && *base != kStackSignature) Fatal("stack overflow in a fiber");
}

void Launch(EntryData& data, const Fiber* fiber) {
    if (fiber->flags & kSetFpuRegs) {
        data.fpucw = 0x037f;
        data.mxcsr = 0x9fc0;
        lbp3_FiberSwitchEntry(&data, true);
    } else {
        lbp3_FiberSwitchEntry(&data, false);
    }
    __builtin_trap();
}

void PrepareContextArea(Fiber* fiber, void* addr, uint64_t size) {
    fiber->contextStart = addr;
    fiber->contextEnd = static_cast<uint8_t*>(addr) + size;
    *static_cast<uint64_t*>(addr) = kStackSignature;
    if (fiber->flags & kContextSizeCheck)
        for (auto* p = static_cast<uint64_t*>(addr) + 1; p < fiber->contextEnd; ++p) *p = kStackSizeCheck;
}

int32_t AttachContext(Fiber* fiber, void* addr, uint64_t size) {
    if (size && size < kContextMinimumSize) return kErrorRange;
    if (size & 15) return kErrorInvalid;
    if (!addr || !size) return kErrorInvalid;
    if (fiber->addrContext) return kErrorInvalid;
    fiber->addrContext = addr;
    fiber->sizeContext = size;
    PrepareContextArea(fiber, addr, size);
    return kOk;
}

[[noreturn]] void SwitchToFiber(Fiber* fiber, uint64_t argOnRunTo, Context* ctx) {
    EnterStackOf(fiber, ctx);
    if (Context* fiberCtx = fiber->context) {
        ctx->argOnRunTo = argOnRunTo;
        lbp3_FiberLongJmp(fiberCtx);
        __builtin_trap();
    }
    EntryData data{};
    if (ctx->prevFiber) {
        Fiber* prev = ctx->prevFiber;
        ctx->prevFiber = nullptr;
        data.state = reinterpret_cast<uint32_t*>(&prev->state);
    }
    data.entry = fiber->entry;
    data.argOnInitialize = fiber->argOnInitialize;
    data.argOnRunTo = argOnRunTo;
    data.stackAddr = static_cast<uint8_t*>(fiber->addrContext) + fiber->sizeContext;
    Launch(data, fiber);
    __builtin_trap();
}

[[noreturn]] void SwitchFiber(Fiber* cur, Fiber* fiber, uint64_t argOnRunTo, Context* ctx) {
    ctx->prevFiber = cur;
    ctx->currentFiber = fiber;
    if (!fiber->addrContext) {
        ctx->prevFiber = nullptr;
        EnterStackOf(nullptr, ctx);
        EntryData data{};
        data.entry = fiber->entry;
        data.argOnInitialize = fiber->argOnInitialize;
        data.argOnRunTo = argOnRunTo;
        data.stackAddr = reinterpret_cast<void*>(ctx->rsp & ~15ull);
        data.state = reinterpret_cast<uint32_t*>(&cur->state);
        Launch(data, fiber);
    }
    SwitchToFiber(fiber, argOnRunTo, ctx);
}

[[noreturn]] void Terminate(uint64_t argOnReturn, Context* ctx) {
    ctx->argOnReturn = argOnReturn;
    EnterStackOf(nullptr, ctx);
    lbp3_FiberLongJmp(ctx);
    __builtin_trap();
}

bool ValidFiber(const Fiber* f) { return f->magicStart == kSignature0 && f->magicEnd == kSignature1; }

}  // namespace

extern "C" void PS4_SYSV_ABI lbp3_FiberForceQuit(uint64_t ret) {
    Context* ctx = CurrentContext();
    ctx->returnVal = ret;
    EnterStackOf(nullptr, ctx);
    lbp3_FiberLongJmp(ctx);
}

int32_t PS4_SYSV_ABI _sceFiberInitializeWithInternalOptionImpl(Fiber* fiber, const char* name, Entry entry,
                                                               uint64_t argOnInitialize, void* addrContext,
                                                               uint64_t sizeContext, const OptParam* opt,
                                                               uint32_t flags, uint32_t buildVer) {
    if (!fiber || !name || !entry) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7 || reinterpret_cast<uintptr_t>(addrContext) & 15)
        return kErrorAlignment;
    if (opt && reinterpret_cast<uintptr_t>(opt) & 7) return kErrorAlignment;
    if (sizeContext && sizeContext < kContextMinimumSize) return kErrorRange;
    if (sizeContext & 15) return kErrorInvalid;
    if (!addrContext != !sizeContext) return kErrorInvalid;
    if (opt && opt->magic != kOptSignature) return kErrorInvalid;

    uint32_t userFlags = flags;
    if (buildVer >= kFw350) userFlags |= kSetFpuRegs;
    if (g_contextSizeCheck) userFlags |= kContextSizeCheck;

    std::strncpy(fiber->name, name, kMaxNameLength);
    fiber->name[kMaxNameLength] = 0;
    fiber->entry = entry;
    fiber->argOnInitialize = argOnInitialize;
    fiber->addrContext = addrContext;
    fiber->sizeContext = sizeContext;
    fiber->context = nullptr;
    fiber->flags = userFlags;
    fiber->magicStart = kSignature0;
    fiber->magicEnd = kSignature1;
    if (addrContext) PrepareContextArea(fiber, addrContext, sizeContext);
    fiber->state = kIdle;
    return kOk;
}

int32_t PS4_SYSV_ABI _sceFiberInitializeImpl(Fiber* fiber, const char* name, Entry entry, uint64_t argOnInitialize,
                                             void* addrContext, uint64_t sizeContext, const OptParam* opt,
                                             uint32_t buildVer) {
    return _sceFiberInitializeWithInternalOptionImpl(fiber, name, entry, argOnInitialize, addrContext, sizeContext,
                                                     opt, 0, buildVer);
}

int32_t PS4_SYSV_ABI sceFiberOptParamInitialize(OptParam* opt) {
    if (!opt) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(opt) & 7) return kErrorAlignment;
    opt->magic = kOptSignature;
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberFinalize(Fiber* fiber) {
    if (!fiber) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7) return kErrorAlignment;
    if (!ValidFiber(fiber)) return kErrorInvalid;
    uint32_t expected = kIdle;
    if (!fiber->state.compare_exchange_strong(expected, kTerminated)) return kErrorState;
    return kOk;
}

int32_t PS4_SYSV_ABI _sceFiberAttachContextAndRun(Fiber* fiber, void* addrContext, uint64_t sizeContext,
                                                  uint64_t argOnRunTo, uint64_t* argOnReturn) {
    if (!fiber) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7 || reinterpret_cast<uintptr_t>(addrContext) & 15)
        return kErrorAlignment;
    if (!ValidFiber(fiber)) return kErrorInvalid;
    core::Tcb* tcb = core::CurrentTcb();
    if (tcb->fiber) return kErrorPermission;
    if (addrContext || sizeContext) {
        const int32_t res = AttachContext(fiber, addrContext, sizeContext);
        if (res < 0) return res;
    }
    uint32_t expected = kIdle;
    if (!fiber->state.compare_exchange_strong(expected, kRun)) return kErrorState;

    // volatile: the setjmp below returns twice and the second return must see
    // the values the fiber side wrote.
    volatile Context ctxStorage{};
    Context& ctx = const_cast<Context&>(ctxStorage);
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    ctx.threadStackBase = tib->StackBase;
    ctx.threadStackLimit = tib->StackLimit;
    ctx.currentFiber = fiber;
    tcb->fiber = &ctx;

    if (!lbp3_FiberSetJmp(&ctx)) {
        if (fiber->addrContext) SwitchToFiber(fiber, argOnRunTo, &ctx);
        EntryData data{};
        data.entry = fiber->entry;
        data.argOnInitialize = fiber->argOnInitialize;
        data.argOnRunTo = argOnRunTo;
        data.stackAddr = reinterpret_cast<void*>(ctx.rsp & ~15ull);
        Launch(data, fiber);
    }

    SetStackBounds(ctxStorage.threadStackBase, ctxStorage.threadStackLimit);
    tcb = core::CurrentTcb();
    Fiber* cur = ctxStorage.currentFiber;
    ctx.currentFiber = nullptr;
    cur->state = kIdle;
    if (ctxStorage.returnVal != 0) Fatal("fiber entry function returned");
    if (argOnReturn) *argOnReturn = ctxStorage.argOnReturn;
    tcb->fiber = nullptr;
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberRun(Fiber* fiber, uint64_t argOnRunTo, uint64_t* argOnReturn) {
    return _sceFiberAttachContextAndRun(fiber, nullptr, 0, argOnRunTo, argOnReturn);
}

int32_t PS4_SYSV_ABI _sceFiberAttachContextAndSwitch(Fiber* fiber, void* addrContext, uint64_t sizeContext,
                                                     uint64_t argOnRunTo, uint64_t* argOnRun) {
    if (!fiber) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7 || reinterpret_cast<uintptr_t>(addrContext) & 15)
        return kErrorAlignment;
    if (!ValidFiber(fiber)) return kErrorInvalid;
    Context* g = CurrentContext();
    if (!g) return kErrorPermission;
    if (addrContext || sizeContext) {
        const int32_t res = AttachContext(fiber, addrContext, sizeContext);
        if (res < 0) return res;
    }
    uint32_t expected = kIdle;
    if (!fiber->state.compare_exchange_strong(expected, kRun)) return kErrorState;

    Fiber* cur = g->currentFiber;
    if (!cur->addrContext) SwitchFiber(cur, fiber, argOnRunTo, g);

    volatile Context ctxStorage{};
    Context& ctx = const_cast<Context&>(ctxStorage);
    if (!lbp3_FiberSetJmp(&ctx)) {
        cur->context = &ctx;
        CheckStackOverflow(g);
        SwitchFiber(cur, fiber, argOnRunTo, g);
    }
    g = CurrentContext();
    if (g->prevFiber) {
        g->prevFiber->state = kIdle;
        g->prevFiber = nullptr;
    }
    if (argOnRun) *argOnRun = g->argOnRunTo;
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberSwitch(Fiber* fiber, uint64_t argOnRunTo, uint64_t* argOnRun) {
    return _sceFiberAttachContextAndSwitch(fiber, nullptr, 0, argOnRunTo, argOnRun);
}

int32_t PS4_SYSV_ABI sceFiberGetSelf(Fiber** fiber) {
    if (!fiber) return kErrorNull;
    Context* g = CurrentContext();
    if (!g) return kErrorPermission;
    *fiber = g->currentFiber;
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberReturnToThread(uint64_t argOnReturn, uint64_t* argOnRun) {
    Context* g = CurrentContext();
    if (!g) return kErrorPermission;
    Fiber* cur = g->currentFiber;
    if (cur->addrContext) {
        volatile Context ctxStorage{};
        Context& ctx = const_cast<Context&>(ctxStorage);
        if (lbp3_FiberSetJmp(&ctx)) {
            g = CurrentContext();
            if (g->prevFiber) {
                g->prevFiber->state = kIdle;
                g->prevFiber = nullptr;
            }
            if (argOnRun) *argOnRun = g->argOnRunTo;
            return kOk;
        }
        cur->context = &ctx;
        CheckStackOverflow(g);
    }
    Terminate(argOnReturn, g);
}

int32_t PS4_SYSV_ABI sceFiberGetInfo(Fiber* fiber, FiberInfo* info) {
    if (!fiber || !info) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7 || reinterpret_cast<uintptr_t>(info) & 7) return kErrorAlignment;
    if (info->size != sizeof(FiberInfo)) return kErrorInvalid;
    if (!ValidFiber(fiber)) return kErrorInvalid;
    info->entry = fiber->entry;
    info->argOnInitialize = fiber->argOnInitialize;
    info->addrContext = fiber->addrContext;
    info->sizeContext = fiber->sizeContext;
    std::strncpy(info->name, fiber->name, kMaxNameLength);
    info->sizeContextMargin = ~0ull;
    if ((fiber->flags & kContextSizeCheck) && fiber->addrContext) {
        const auto* start = static_cast<const uint64_t*>(fiber->contextStart);
        const auto* end = static_cast<const uint64_t*>(fiber->contextEnd);
        uint64_t margin = 0;
        if (*start == kStackSignature) {
            const uint64_t* p = start + 1;
            while (p < end && *p == kStackSizeCheck) ++p;
            margin = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(start + 1);
        }
        info->sizeContextMargin = margin;
    }
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberStartContextSizeCheck(uint32_t flags) {
    if (flags != 0) return kErrorInvalid;
    uint32_t expected = 0;
    return g_contextSizeCheck.compare_exchange_strong(expected, 1u) ? kOk : kErrorState;
}

int32_t PS4_SYSV_ABI sceFiberStopContextSizeCheck() {
    uint32_t expected = 1;
    return g_contextSizeCheck.compare_exchange_strong(expected, 0u) ? kOk : kErrorState;
}

int32_t PS4_SYSV_ABI sceFiberRename(Fiber* fiber, const char* name) {
    if (!fiber || !name) return kErrorNull;
    if (reinterpret_cast<uintptr_t>(fiber) & 7) return kErrorAlignment;
    if (!ValidFiber(fiber)) return kErrorInvalid;
    std::strncpy(fiber->name, name, kMaxNameLength);
    return kOk;
}

int32_t PS4_SYSV_ABI sceFiberGetThreadFramePointerAddress(uint64_t* addr) {
    if (!addr) return kErrorNull;
    Context* g = CurrentContext();
    if (!g) return kErrorPermission;
    *addr = g->rbp;
    return kOk;
}

}  // namespace hle::fiber

namespace hle {

void RegisterFiber(core::Linker& linker) {
    using namespace fiber;
    const char* lib = "libSceFiber";
    HLE_REG(linker, lib, _sceFiberInitializeImpl);
    HLE_REG(linker, lib, _sceFiberInitializeWithInternalOptionImpl);
    HLE_REG(linker, lib, sceFiberOptParamInitialize);
    HLE_REG(linker, lib, sceFiberFinalize);
    HLE_REG(linker, lib, sceFiberRun);
    HLE_REG(linker, lib, sceFiberSwitch);
    HLE_REG(linker, lib, sceFiberGetSelf);
    HLE_REG(linker, lib, sceFiberReturnToThread);
    HLE_REG(linker, lib, _sceFiberAttachContextAndRun);
    HLE_REG(linker, lib, _sceFiberAttachContextAndSwitch);
    HLE_REG(linker, lib, sceFiberGetInfo);
    HLE_REG(linker, lib, sceFiberStartContextSizeCheck);
    HLE_REG(linker, lib, sceFiberStopContextSizeCheck);
    HLE_REG(linker, lib, sceFiberRename);
    HLE_REG(linker, lib, sceFiberGetThreadFramePointerAddress);
}

}  // namespace hle
