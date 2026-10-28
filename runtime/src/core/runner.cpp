// Entry sequence adapted from shadPS4 (GPL-2.0-or-later), src/core/linker.cpp.
#include "core/runner.h"
#include "core/linker.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/native_trace.h"
#include "input/game_state.h"
#include "core/tls.h"
#include "hle/abi.h"

namespace hle::kernel {
void CommitWholeStack();  // kernel_pthread.cpp
}

namespace core {
namespace {

constexpr size_t kMainStackSize = 16u << 20;
constexpr size_t kRecentStubs = 16;

struct EntryParams {
    int argc;
    uint32_t padding;
    const char* argv[33];
    uint64_t entryAddr;
};

using ModuleStartFn = int(PS4_SYSV_ABI*)(size_t args, const void* argp, void* param);

std::vector<Module*> g_modules;
std::mutex g_symMutex;  // DbgHelp is single-threaded
bool g_symReady = false;
const HMODULE g_self = GetModuleHandleW(nullptr);
Module* g_eboot = nullptr;
std::vector<Module*> g_startFirst;
EntryParams g_params{};

struct ThreadInfo { HANDLE handle; DWORD tid; std::string name; };
std::mutex g_threadsMutex;
std::vector<ThreadInfo> g_threads;
int g_dumpAfter = 0;

std::recursive_mutex g_startMutex;  // module inits may load further modules

std::mutex g_stubMutex;
std::array<std::string, kRecentStubs> g_recent;
std::map<std::string, uint64_t> g_stubCounts;
size_t g_recentCount = 0;

void PS4_SYSV_ABI ProgramExit() {
    std::fprintf(stderr, "[exit] game called its exit handler\n");
    std::fflush(stderr);
    ExitProcess(0);
}

std::string Describe(uint64_t addr) {
    for (Module* m : g_modules) {
        if (m->Contains(reinterpret_cast<void*>(addr))) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", m->fileName.c_str(),
                          static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(m->base)));
            return buf;
        }
    }
    // Our own code: function name from the PDB.
    {
        std::lock_guard lock(g_symMutex);
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (g_symReady && SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
            char buf[320];
            std::snprintf(buf, sizeof(buf), "%s+0x%llx (runtime)", sym->Name, static_cast<unsigned long long>(disp));
            return buf;
        }
    }
    // Other host code: name the DLL it's in.
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(mod, full, MAX_PATH)) {
            const char* slash = std::strrchr(full, '\\');
            std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : full);
        }
    }
    char buf[MAX_PATH + 48];
    std::snprintf(buf, sizeof(buf), "%s+0x%llx (host)", name,
                  static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(mod)));
    return buf;
}

// Prints values near the top of a stack that point into game code or into
// the runtime (likely return addresses; a few may be stale).
// `snapshot`, if given, is a copy of the stack starting at rsp (used for other
// threads, which keep running while we print).
void PrintGameFrames(uint64_t rsp, uint64_t stackHigh, const uint64_t* snapshot = nullptr) {
    auto* sp = snapshot ? snapshot : reinterpret_cast<const uint64_t*>(rsp);
    int shown = 0;
    const uint64_t words = stackHigh > rsp ? (stackHigh - rsp) / 8 : 0;
    for (uint64_t i = 0; i < 2048 && i < words && shown < 16; ++i) {
        const uint64_t v = sp[i];
        bool interesting = false;
        for (Module* m : g_modules) interesting |= m->Contains(reinterpret_cast<void*>(v));
        HMODULE mod = nullptr;
        if (!interesting &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(v), &mod))
            interesting = mod == g_self;
        if (!interesting) continue;
        std::fprintf(stderr, "  [rsp+0x%03x] %s\n", static_cast<unsigned>(i * 8), Describe(v).c_str());
        ++shown;
    }
}

// Upper end of another thread's stack, from its TEB.
uint64_t StackHighOf(HANDLE thread) {
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    struct { LONG status; PVOID tebBase; ULONG_PTR rest[4]; } basic{};
    if (!query || query(thread, 0 /*ThreadBasicInformation*/, &basic, sizeof(basic), nullptr) != 0) return 0;
    return reinterpret_cast<uint64_t>(static_cast<const NT_TIB*>(basic.tebBase)->StackBase);
}

// First-chance C++ exceptions: report where they're thrown (a few times), then
// let normal handling continue. Useful when a catch-all turns them into a crash.
LONG CALLBACK CxxThrowReporter(EXCEPTION_POINTERS* ep) {
    constexpr DWORD kMsvcCxxException = 0xE06D7363;
    static std::atomic<int> reported{0};
    if (ep->ExceptionRecord->ExceptionCode != kMsvcCxxException || reported++ >= 3) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "\n=== C++ exception thrown (first chance) ===\n");
    ULONG_PTR stackLow = 0, stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    PrintGameFrames(ep->ContextRecord->Rsp, stackHigh);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_GUARD_PAGE:  // a stack guard page touched outside normal stack growth
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // A fault inside this handler would otherwise re-enter it.
    // Another thread crashing at the same time waits, so the first report is
    // written in full before the process ends.
    static std::atomic<DWORD> handler{0};
    DWORD expected = 0;
    if (!handler.compare_exchange_strong(expected, GetCurrentThreadId())) {
        if (expected == GetCurrentThreadId()) ExitProcess(3);
        Sleep(INFINITE);
    }
    const CONTEXT& c = *ep->ContextRecord;
    std::fprintf(stderr, "\n=== crash: exception 0x%08lX at %s ===\n", code, Describe(c.Rip).c_str());
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const auto& info = ep->ExceptionRecord->ExceptionInformation;
        static const char* const kKinds[] = {"read", "write", "?", "?", "?", "?", "?", "?", "execute"};
        std::fprintf(stderr, "%s of address 0x%llx\n", kKinds[info[0] < 9 ? info[0] : 2],
                     static_cast<unsigned long long>(info[1]));
    }
    std::fprintf(stderr, "rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", c.Rax, c.Rbx, c.Rcx, c.Rdx);
    std::fprintf(stderr, "rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    std::fprintf(stderr, "r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", c.R8, c.R9, c.R10, c.R11);

    std::fprintf(stderr, "stack (game code addresses):\n");
    ULONG_PTR stackLow = 0, stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    PrintGameFrames(c.Rsp, stackHigh);

    std::lock_guard lock(g_stubMutex);
    std::fprintf(stderr, "last unimplemented calls (oldest first):\n");
    size_t n = std::min(g_recentCount, kRecentStubs);
    for (size_t i = 0; i < n; ++i)
        std::fprintf(stderr, "  %s\n", g_recent[(g_recentCount - n + i) % kRecentStubs].c_str());
    std::fflush(stderr);
    ExitProcess(3);
}

[[noreturn]] void JumpToEntry(EntryParams* params) {
    // Same stack shape the PS4 kernel hands the process: EntryParams copied to
    // the top, rdi = params, rsi = exit handler. There's no coming back.
    asm volatile(
        "andq $-16, %%rsp\n"
        "subq $8, %%rsp\n"
        "pushq 8(%1)\n"
        "pushq 0(%1)\n"
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "jmp *%0\n"
        :
        : "r"(params->entryAddr), "r"(params), "r"(&ProgramExit)
        : "rax", "rsi", "rdi");
    __builtin_unreachable();
}

DWORD WINAPI MainThread(void*) {
    hle::kernel::CommitWholeStack();
    SetupThreadTls();
    RegisterCurrentThread("main");
    for (Module* m : g_startFirst) StartModule(*m, 0, nullptr);
    static const char* const kArgv0 = "/app0/eboot.bin";
    g_params.argc = 1;
    g_params.argv[0] = kArgv0;
    g_params.entryAddr = reinterpret_cast<uint64_t>(g_eboot->Addr(g_eboot->entry));
    std::fprintf(stderr, "[entry] jumping to %s\n", Describe(g_params.entryAddr).c_str());
    JumpToEntry(&g_params);
}

}  // namespace

Module* FindLoadedModule(const std::string& fileName) {
    for (Module* m : g_modules)
        if (_stricmp(m->fileName.c_str(), fileName.c_str()) == 0) return m;
    return nullptr;
}

int ModuleHandle(const Module* m) {
    for (size_t i = 0; i < g_modules.size(); ++i)
        if (g_modules[i] == m) return static_cast<int>(i) + 1;
    return 0;
}

int StartModule(Module& m, size_t args, const void* argp) {
    std::lock_guard lock(g_startMutex);
    if (m.started) return 0;
    m.started = true;
    if (!m.hasInit) return 0;
    std::fprintf(stderr, "[start] %s\n", m.fileName.c_str());
    int ret = reinterpret_cast<ModuleStartFn>(m.Addr(m.init))(args, argp, nullptr);
    std::fprintf(stderr, "[start] %s returned %d\n", m.fileName.c_str(), ret);
    return ret;
}

void RegisterCurrentThread(const std::string& name) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lock(g_threadsMutex);
    g_threads.push_back({h, GetCurrentThreadId(), name});
}

void UnregisterCurrentThread() {
    std::lock_guard lock(g_threadsMutex);
    const DWORD me = GetCurrentThreadId();
    for (auto it = g_threads.begin(); it != g_threads.end(); ++it) {
        if (it->tid != me) continue;
        CloseHandle(it->handle);
        g_threads.erase(it);
        return;
    }
}

std::string DescribeAddress(uint64_t addr) { return Describe(addr); }
uint8_t* EbootBase() { return g_eboot ? g_eboot->base : nullptr; }
Module* EbootModule() { return g_eboot; }

std::string ThreadName(unsigned long tid) {
    std::lock_guard lock(g_threadsMutex);
    for (const auto& t : g_threads)
        if (t.tid == tid) return t.name;
    return tid ? "?" : "nobody";
}

void DumpAllThreads() {
    {
        // First, so it survives even if walking the threads below gets stuck.
        std::lock_guard stubLock(g_stubMutex);
        std::vector<std::pair<uint64_t, std::string>> top;
        for (const auto& [name, n] : g_stubCounts) top.emplace_back(n, name);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "most-called unimplemented functions:\n");
        for (size_t i = 0; i < top.size() && i < 12; ++i)
            std::fprintf(stderr, "  %10llu  %s\n", static_cast<unsigned long long>(top[i].first), top[i].second.c_str());
    }
    PrintCallCounts(30);
    PrintNativeCounts(400);
    // LBP3's sound instances: a table of 0xA0-byte entries (pointer at eboot+0x1329878);
    // bit 4 of byte +0x6d is what the script native GetIsPlaying reports. List the entries
    // still marked playing (a cutscene waits on one that never stops).
    if (g_eboot && std::getenv("LBP3_DUMP_SOUNDS")) {
        uint64_t table = 0;
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), g_eboot->Addr(0x1329878), &table, 8, &got);
        std::fprintf(stderr, "[sounds] table at %llx\n", static_cast<unsigned long long>(table));
        for (int i = 0; table && i < 4096; ++i) {
            uint8_t e[0xA0];
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(table + i * 0xA0ull), e, sizeof(e),
                                   &got))
                break;
            if (!(e[0x6d] & 0x10)) continue;
            std::string hex;
            for (int k = 0; k < 0xA0; ++k) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02x", e[k]);
                hex += b;
                if (k % 8 == 7) hex += ' ';
            }
            std::fprintf(stderr, "[sounds] #%d playing: %s\n", i + 1, hex.c_str());
        }
    }
    // LBP3_WATCH=off,off,...: print 16 bytes at each eboot offset (game globals).
    if (const char* v = std::getenv("LBP3_WATCH"); v && g_eboot) {
        std::string list = v;
        size_t pos = 0;
        while (pos < list.size()) {
            size_t comma = list.find(',', pos);
            if (comma == std::string::npos) comma = list.size();
            // "*off+add": read the pointer at eboot+off, then look at pointer+add.
            std::string item = list.substr(pos, comma - pos);
            const bool deref = !item.empty() && item[0] == '*';
            if (deref) item.erase(0, 1);
            const size_t plus = item.find('+');
            const uint64_t off = std::strtoull(item.substr(0, plus).c_str(), nullptr, 16);
            const uint64_t add = plus == std::string::npos ? 0 : std::strtoull(item.c_str() + plus + 1, nullptr, 16);
            uint64_t q[2] = {};
            SIZE_T got = 0;
            const void* where = g_eboot->Addr(off);
            if (deref) {
                uint64_t ptr = 0;
                ReadProcessMemory(GetCurrentProcess(), where, &ptr, sizeof(ptr), &got);
                where = reinterpret_cast<const void*>(ptr + add);
            }
            ReadProcessMemory(GetCurrentProcess(), where, q, sizeof(q), &got);
            std::fprintf(stderr, "[watch] eboot+0x%llx: %016llx %016llx (as int32: %d, %d)\n",
                         static_cast<unsigned long long>(off), static_cast<unsigned long long>(q[0]),
                         static_cast<unsigned long long>(q[1]), static_cast<int32_t>(q[0]),
                         static_cast<int32_t>(q[0] >> 32));
            pos = comma + 1;
        }
    }
    std::lock_guard lock(g_threadsMutex);
    std::fprintf(stderr, "\n=== thread dump (%zu threads) ===\n", g_threads.size());
    // While a thread is suspended, only copy its registers and stack: it may hold
    // a heap or stdio lock, and printing then would deadlock the dump.
    std::vector<uint64_t> stack(2048);
    // Progress marks go straight to the handle: no CRT locks, so a stuck step shows.
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    auto mark = [err](const char* s) {
        DWORD n;
        WriteFile(err, s, static_cast<DWORD>(std::strlen(s)), &n, nullptr);
    };
    for (const auto& t : g_threads) {
        mark("[dump] stack base\n");
        const uint64_t high = StackHighOf(t.handle);
        mark("[dump] suspend\n");
        if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        mark("[dump] context\n");
        const bool ok = GetThreadContext(t.handle, &c) != 0;
        size_t words = 0;
        mark("[dump] copy\n");
        if (ok && high > c.Rsp) {
            words = std::min<size_t>(stack.size(), (high - c.Rsp) / 8);
            // ReadProcessMemory fails instead of faulting (a fault here would go
            // through exception handlers whose locks the suspended thread may hold).
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack.data(), words * 8,
                                   &got))
                words = got / 8;
        }
        ResumeThread(t.handle);
        if (!ok) continue;
        // Raw address first: symbol lookup (DbgHelp) needs the loader lock, which a
        // stuck thread may be holding.
        std::fprintf(stderr, "-- %s: rip=%016llx rsp=%016llx\n", t.name.c_str(),
                     static_cast<unsigned long long>(c.Rip), static_cast<unsigned long long>(c.Rsp));
        std::fprintf(stderr, "   at %s\n", Describe(c.Rip).c_str());
        if (words) PrintGameFrames(c.Rsp, c.Rsp + words * 8, stack.data());
    }

    // Threads we didn't register (renderer, SDL, ...) that have a name.
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            bool known = false;
            for (const auto& t : g_threads) known |= t.tid == te.th32ThreadID;
            if (known) continue;
            HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
            if (!h) continue;
            PWSTR wname = nullptr;
            std::string name;
            if (SUCCEEDED(GetThreadDescription(h, &wname)) && wname) {
                for (PWSTR p = wname; *p; ++p) name += static_cast<char>(*p);
                LocalFree(wname);
            }
            if (!name.empty() && SuspendThread(h) != static_cast<DWORD>(-1)) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(h, &c)) {
                    std::fprintf(stderr, "-- [host] %s: at %s\n", name.c_str(), Describe(c.Rip).c_str());
                    if (uint64_t high = StackHighOf(h)) PrintGameFrames(c.Rsp, high);
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
    }

    std::fflush(stderr);
}

void SetDumpAfter(int seconds) { g_dumpAfter = seconds; }

void* ProcParam() {
    return g_eboot && g_eboot->procParam ? g_eboot->Addr(g_eboot->procParam) : nullptr;
}

void NoteStubCall(const std::string& lib, const std::string& name) {
    std::lock_guard lock(g_stubMutex);
    std::string key = lib + " :: " + name;
    ++g_stubCounts[key];
    g_recent[g_recentCount++ % kRecentStubs] = std::move(key);
}

// Samples a thread (game thread name, or a host thread's description) and
// writes where it spends its time: game-code hot spots, frame-pointer chains,
// and unwound host stacks for time spent outside game code.
void SampleThreadReport(const std::string& target, int samples, FILE* out) {
    HANDLE h = nullptr;
    {
        std::lock_guard lock(g_threadsMutex);
        for (const auto& t : g_threads)
            if (t.name == target) h = t.handle;
    }
    if (!h) {  // not a game thread: look for a host thread with that description
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok && !h; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                if (!th) continue;
                PWSTR desc = nullptr;
                if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                    std::string name;
                    for (const wchar_t* p = desc; *p; ++p) name += static_cast<char>(*p);
                    LocalFree(desc);
                    if (name.find(target) != std::string::npos) h = th;
                }
                if (h != th) CloseHandle(th);
            }
            CloseHandle(snap);
        }
    }
    if (!h) {
        std::fprintf(out, "[sample] no thread named '%s'\n", target.c_str());
        return;
    }
    const uint64_t high = StackHighOf(h);
    std::vector<uint64_t> stack(4096);
    std::map<uint64_t, int> hits, rips;
    std::map<std::string, int> chains, hostChains;
    std::map<uint64_t, std::string> names;
    // Only samples taken while the thread runs game code count: waits in
    // the runtime (flip pacing, sleeps) would otherwise drown everything.
    auto inCode = [](uint64_t a) {
        if (!g_eboot) return false;
        for (auto [base, size] : g_eboot->CodeRanges())
            if (a >= reinterpret_cast<uint64_t>(base) && a < reinterpret_cast<uint64_t>(base) + size)
                return true;
        return false;
    };
    const int kSamples = samples;
    int inGame = 0;
    // Two samplers (F10 samples main and GPU threads at once) must not both have a
    // thread suspended: one can stop the thread holding a lock the other's
    // stack walk needs.
    static std::mutex sampleMutex;
    for (int s = 0; s < kSamples; ++s) {
        std::unique_lock suspended(sampleMutex);
        if (SuspendThread(h) == static_cast<DWORD>(-1)) break;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        size_t words = 0;
        if (GetThreadContext(h, &c) && high > c.Rsp) {
            words = std::min<size_t>(stack.size(), (high - c.Rsp) / 8);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), stack.data(),
                                   words * 8, &got))
                words = got / 8;
        }
        // Outside game code: unwind the host stack while still suspended.
        std::array<uint64_t, 10> host{};
        size_t hostFrames = 0;
        if (!inCode(c.Rip)) {
            CONTEXT walk = c;
            STACKFRAME64 frame{};
            frame.AddrPC = {walk.Rip, 0, AddrModeFlat};
            frame.AddrStack = {walk.Rsp, 0, AddrModeFlat};
            frame.AddrFrame = {walk.Rbp, 0, AddrModeFlat};
            while (hostFrames < host.size() &&
                   StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), h, &frame, &walk, nullptr,
                               SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
                   frame.AddrPC.Offset)
                host[hostFrames++] = frame.AddrPC.Offset;
        }
        ResumeThread(h);
        suspended.unlock();
        if (hostFrames) {
            std::string chain;
            for (size_t i = 0; i < hostFrames; ++i) {
                auto it = names.find(host[i]);
                if (it == names.end()) it = names.emplace(host[i], Describe(host[i])).first;
                chain += (i ? " < " : "") + it->second;
                if (inCode(host[i])) break;  // reached the game code that called in
            }
            ++hostChains[chain];
        }
        if (inCode(c.Rip)) {
            ++inGame;
            ++rips[c.Rip & ~uint64_t{0xF}];
            // Walk the frame-pointer chain (game code keeps rbp frames):
            // [rbp] = caller's rbp, [rbp+8] = return address. Exact, unlike
            // scanning the stack for code-looking values.
            std::string chain;
            uint64_t fp = c.Rbp;
            for (int depth = 0; depth < 24; ++depth) {
                if (fp < c.Rsp || fp + 16 > c.Rsp + words * 8) break;
                const size_t idx = (fp - c.Rsp) / 8;
                const uint64_t ret = stack[idx + 1];
                if (!inCode(ret)) break;
                ++hits[ret];
                char buf[24];
                std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(ret - reinterpret_cast<uint64_t>(g_eboot->base)));
                chain += buf;
                const uint64_t next = stack[idx];
                if (next <= fp) break;
                fp = next;
            }
            ++chains[chain];
        }
        Sleep(1);
    }
    auto print = [out](const char* title, const std::map<uint64_t, int>& m) {
        std::vector<std::pair<int, uint64_t>> top;
        for (auto [a, n] : m) top.emplace_back(n, a);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(out, "[sample] %s\n", title);
        for (size_t i = 0; i < top.size() && i < 40; ++i)
            std::fprintf(out, "[sample] %5d  %s\n", top[i].first, Describe(top[i].second).c_str());
    };
    std::fprintf(out, "[sample] '%s': %d of %d samples were in game code\n", target.c_str(), inGame,
                 kSamples);
    print("where it was executing:", rips);
    print("callers (frame-pointer chain):", hits);
    std::vector<std::pair<int, std::string>> top;
    for (const auto& [ch, n] : chains) top.emplace_back(n, ch);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 12; ++i)
        std::fprintf(out, "[sample] chain x%d:%s\n", top[i].first, top[i].second.c_str());
    top.clear();
    for (const auto& [ch, n] : hostChains) top.emplace_back(n, ch);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 15; ++i)
        std::fprintf(out, "[sample] host x%d: %s\n", top[i].first, top[i].second.c_str());
}

// Shows a Windows toast notification (through a hidden PowerShell, using the toast API
// Windows ships with). Text must not contain single quotes.
void ShowToast(const std::string& title, const std::string& text) {
    const std::string script =
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, "
        "ContentType = WindowsRuntime] > $null; "
        "$x = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent("
        "[Windows.UI.Notifications.ToastTemplateType]::ToastText02); "
        "$t = $x.GetElementsByTagName('text'); "
        "$t.Item(0).AppendChild($x.CreateTextNode('" + title + "')) > $null; "
        "$t.Item(1).AppendChild($x.CreateTextNode('" + text + "')) > $null; "
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("
        "'{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe')"
        ".Show([Windows.UI.Notifications.ToastNotification]::new($x))";
    std::string cmd = "powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"" + script + "\"";
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void RequestPerfReport() {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;  // one report at a time
    std::thread([] {
        static int n = 0;
        CreateDirectoryA("analysis\\perf", nullptr);
        char name[64];
        std::snprintf(name, sizeof(name), "analysis\\perf\\perf%02d.txt", ++n);
        // Where every thread is right now first (into the log): shows what a stuck game waits on.
        std::fprintf(stderr, "[perf] all threads:\n");
        DumpAllThreads();
        std::fprintf(stderr, "[perf] sampling main and GPU threads for %s\n", name);
        FILE* mainOut = std::tmpfile();
        FILE* gpuOut = std::tmpfile();
        std::thread gpu([gpuOut] { SampleThreadReport("GpuCommandProcessor", 3000, gpuOut); });
        SampleThreadReport("main", 3000, mainOut);
        gpu.join();
        if (FILE* f = std::fopen(name, "w")) {
            for (FILE* part : {mainOut, gpuOut}) {
                std::rewind(part);
                char buf[4096];
                size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), part)) > 0) std::fwrite(buf, 1, got, f);
                std::fputs("\n", f);
            }
            std::fclose(f);
        }
        std::fclose(mainOut);
        std::fclose(gpuOut);
        std::fprintf(stderr, "[perf] wrote %s\n", name);
        std::fflush(stderr);
        ShowToast("LittleBigPlanet 3", std::string("F10 report finished (") + name + "). You can close the game now.");
        busy = false;
    }).detach();
}

// LBP3_FIND_REFS=text,text,...: for each string containing that text, list the data
// slots the loader points at it (relocations), with the pointers stored next to them:
// finds name -> function tables, such as the script language's native bindings.
void FindStringRefs(Module& m, const char* list) {
    std::string items = list;
    for (size_t pos = 0; pos < items.size();) {
        size_t comma = items.find(',', pos);
        if (comma == std::string::npos) comma = items.size();
        const std::string text = items.substr(pos, comma - pos);
        pos = comma + 1;
        const auto* begin = m.base;
        const auto* end = m.base + m.size;
        for (const uint8_t* p = begin; (p = std::search(p, end, text.begin(), text.end())) != end; ++p) {
            const uint8_t* s = p;
            while (s > begin && s[-1] >= 0x20 && s[-1] < 0x7f) --s;  // start of the string
            const uint64_t target = static_cast<uint64_t>(s - begin);
            std::fprintf(stderr, "[refs] \"%s\" at eboot+0x%llx\n", reinterpret_cast<const char*>(s),
                         static_cast<unsigned long long>(target));
            for (const auto& r : m.relas) {
                if (static_cast<uint64_t>(r.addend) != target) continue;
                std::string around;
                for (const auto& n : m.relas) {
                    const int64_t delta = static_cast<int64_t>(n.offset) - static_cast<int64_t>(r.offset);
                    if (delta < -16 || delta > 24 || delta == 0) continue;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), " [%+lld: %s]", static_cast<long long>(delta),
                                  Describe(reinterpret_cast<uint64_t>(m.base + n.addend)).c_str());
                    around += buf;
                }
                std::fprintf(stderr, "[refs]   slot eboot+0x%llx;%s\n", static_cast<unsigned long long>(r.offset),
                             around.c_str());
            }
            p = s + std::strlen(reinterpret_cast<const char*>(s));
        }
    }
    std::fflush(stderr);
}

void Run(Module& eboot, const std::vector<Module*>& startFirst, const std::vector<Module*>& allModules) {
    g_eboot = &eboot;
    g_startFirst = startFirst;
    g_modules = allModules;
    if (const char* v = std::getenv("LBP3_FIND_REFS")) FindStringRefs(eboot, v);
    InstallNativeCounters(eboot);
    input::game_state::InstallHooks();  // keyboard actions' hooks, before the game starts
    AddVectoredExceptionHandler(0, CrashHandler);  // last, after breakpoints
    AddVectoredExceptionHandler(0, CxxThrowReporter);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    g_symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    // LBP3_SAMPLE=<thread name>:<seconds>: after that many seconds, snapshot the
    // thread's stack 150 times (every 20 ms) and print the game-code addresses
    // found most often: the call chain the thread keeps running.
    if (const char* v = std::getenv("LBP3_SAMPLE")) {
        std::string spec = v;
        const size_t colon = spec.rfind(':');
        const std::string target = spec.substr(0, colon);
        const int seconds = colon == std::string::npos ? 60 : std::atoi(spec.c_str() + colon + 1);
        std::thread([target, seconds] {
            Sleep(static_cast<DWORD>(seconds) * 1000);
            SampleThreadReport(target, 3000, stderr);
        }).detach();
    }
    if (const char* v = std::getenv("LBP3_COUNT_CALLS")) {
        const int seconds = std::atoi(v);
        std::thread([seconds] {
            Sleep(static_cast<DWORD>(seconds) * 1000);
            ResetCallCounts();
            std::fputs("[count] call counters reset\n", stderr);
        }).detach();
    }
    if (g_dumpAfter > 0) {
        std::thread([] {
            Sleep(static_cast<DWORD>(g_dumpAfter) * 1000);
            DumpAllThreads();
            std::fputs("=== dump complete ===\n", stderr);
            ExitProcess(5);
        }).detach();
    }
    // LBP3_PERF_AFTER=seconds: an F10 report that long after launch (for slow boots).
    if (const char* v = std::getenv("LBP3_PERF_AFTER")) {
        const int seconds = std::atoi(v);
        std::thread([seconds] {
            Sleep(static_cast<DWORD>(seconds) * 1000);
            RequestPerfReport();
        }).detach();
    }

    HANDLE t = CreateThread(nullptr, kMainStackSize, MainThread, nullptr,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) {
        std::fprintf(stderr, "cannot create main thread\n");
        ExitProcess(1);
    }
    WaitForSingleObject(t, INFINITE);
    ExitProcess(0);
}

}  // namespace core
