#include "core/linker.h"
#include "core/patches.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>
#include <cstring>
#include <random>
#include <stdexcept>

#include "core/nid.h"
#include "core/runner.h"
#include "hle/abi.h"

namespace core {
namespace {

constexpr uint32_t R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7;
constexpr uint32_t R_X86_64_RELATIVE = 8, R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17;
constexpr uint32_t R_X86_64_TPOFF64 = 18;
constexpr uint8_t STT_OBJECT = 1, STT_TLS = 6;
constexpr uint8_t STB_WEAK = 2;

constexpr size_t kThunkArenaSize = 1 << 20;
constexpr size_t kThunkSize = 22;
// Unresolved variables get zeroed storage of this size.
constexpr size_t kDataStubSize = 256;
// Log the first few calls to each stub, then go quiet.
constexpr uint64_t kStubLogLimit = 3;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// Splits "NID#L#M" into its parts. Returns false for plain names.
bool SplitSymbol(const char* s, std::string& nid, std::string& lib) {
    const char* h1 = std::strchr(s, '#');
    if (!h1) return false;
    const char* h2 = std::strchr(h1 + 1, '#');
    if (!h2) return false;
    nid.assign(s, h1);
    lib.assign(h1 + 1, h2);
    return true;
}

// --- call counting (LBP3_COUNT_CALLS=seconds) ------------------------------------------
// Each HLE import goes through a thunk that bumps a counter, so a dump can show
// what the game keeps calling while it waits on something.

struct CallCounter {
    std::string lib, name;
    std::atomic<uint64_t> calls{0};
};
std::deque<CallCounter> g_callCounters;  // deque: stable addresses for the thunks
std::mutex g_callCountersMutex;
constexpr size_t kCountThunkSize = 26;

bool CountCalls() {
    static const bool on = std::getenv("LBP3_COUNT_CALLS") != nullptr;
    return on;
}

}  // namespace

void ResetCallCounts() {
    std::lock_guard lk(g_callCountersMutex);
    for (auto& c : g_callCounters) c.calls = 0;
}

void PrintCallCounts(int top) {
    std::lock_guard lk(g_callCountersMutex);
    if (g_callCounters.empty()) return;
    std::vector<std::pair<uint64_t, const CallCounter*>> v;
    for (const auto& c : g_callCounters)
        if (c.calls) v.emplace_back(c.calls.load(), &c);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "most-called system functions (since the counter reset):\n");
    for (int i = 0; i < top && i < static_cast<int>(v.size()); ++i)
        std::fprintf(stderr, "  %10llu  %s :: %s\n", static_cast<unsigned long long>(v[i].first),
                     v[i].second->lib.c_str(), v[i].second->name.c_str());
}

// mov rax, &counter ; lock inc qword [rax] ; mov rax, target ; jmp rax
uint64_t Linker::CountingThunk(const std::string& lib, const std::string& name, uint64_t target) {
    if (thunkUsed_ + kCountThunkSize > kThunkArenaSize) return target;
    CallCounter* c;
    {
        std::lock_guard lk(g_callCountersMutex);
        c = &g_callCounters.emplace_back();
        c->lib = lib;
        c->name = name;
    }
    uint8_t* t = thunkArena_ + thunkUsed_;
    const uint64_t cp = reinterpret_cast<uint64_t>(&c->calls);
    t[0] = 0x48; t[1] = 0xB8; std::memcpy(t + 2, &cp, 8);
    t[10] = 0xF0; t[11] = 0x48; t[12] = 0xFF; t[13] = 0x00;
    t[14] = 0x48; t[15] = 0xB8; std::memcpy(t + 16, &target, 8);
    t[24] = 0xFF; t[25] = 0xE0;
    thunkUsed_ += kCountThunkSize;
    return reinterpret_cast<uint64_t>(t);
}

uint64_t PS4_SYSV_ABI UnimplementedCall(Linker::Stub* s) {
    NoteStubCall(s->lib, s->name);
    if (s->calls++ < kStubLogLimit)
        std::fprintf(stderr, "[stub] %s :: %s\n", s->lib.c_str(), s->name.c_str());
    return 0;
}

Linker::Linker() {
    thunkArena_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kThunkArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!thunkArena_) throw std::runtime_error("cannot allocate stub thunks");
}

void Linker::RegisterHle(const std::string& lib, const std::string& name, void* fn) {
    std::string nid = Nid(name);
    names_[nid] = name;
    hle_[nid + "#" + lib] = reinterpret_cast<uint64_t>(fn);
}

void Linker::RegisterHleNid(const std::string& lib, const std::string& nid, void* fn) {
    hle_[nid + "#" + lib] = reinterpret_cast<uint64_t>(fn);
}

void Linker::AddLibraryAlias(const std::string& from, const std::string& to) { aliases_[from] = to; }

void Linker::AddNameHint(const std::string& name) { names_[Nid(name)] = name; }

std::string Linker::Pretty(const std::string& nid) const {
    auto it = names_.find(nid);
    return it != names_.end() ? it->second : nid;
}

void Linker::AddModule(Module* m) {
    modules_.push_back(m);
    for (const auto& s : m->syms) {
        if (s.shndx == 0 || s.Bind() == 0) continue;  // undefined or local
        std::string nid, libEnc;
        if (!SplitSymbol(m->Str(s.name), nid, libEnc)) continue;
        auto lib = m->exportLibs.find(DecodeId(libEnc));
        if (lib == m->exportLibs.end()) continue;
        exports_.emplace(nid + "#" + lib->second, reinterpret_cast<uint64_t>(m->Addr(s.value)));
    }
    if (m->tls.memSize) {
        m->tls.index = nextTlsIndex_++;
        nextTlsOffset_ = AlignUp(nextTlsOffset_ + m->tls.memSize, m->tls.align);
        m->tls.offset = nextTlsOffset_;
    }
}

void Linker::LinkAll() {
    for (Module* m : modules_) Relocate(*m);
    for (Module* m : modules_) ApplyPatches(*m);
    for (Module* m : modules_) m->Protect();
}

uint64_t Linker::MakeStub(const std::string& lib, const std::string& name) {
    std::string key = name + "#" + lib;
    if (auto it = stubCache_.find(key); it != stubCache_.end()) return it->second;
    if (thunkUsed_ + kThunkSize > kThunkArenaSize) throw std::runtime_error("stub arena full");

    Stub* s = &stubs_.emplace_back(Stub{lib, name});
    // mov rdi, s ; mov rax, UnimplementedCall ; jmp rax
    uint8_t* t = thunkArena_ + thunkUsed_;
    uint64_t sp = reinterpret_cast<uint64_t>(s);
    uint64_t fp = reinterpret_cast<uint64_t>(&UnimplementedCall);
    t[0] = 0x48; t[1] = 0xBF; std::memcpy(t + 2, &sp, 8);
    t[10] = 0x48; t[11] = 0xB8; std::memcpy(t + 12, &fp, 8);
    t[20] = 0xFF; t[21] = 0xE0;
    thunkUsed_ += kThunkSize;

    uint64_t addr = reinterpret_cast<uint64_t>(t);
    stubCache_[key] = addr;
    return addr;
}

uint64_t Linker::ResolveImport(const Module& m, const ElfSym& sym) {
    std::string nid, libEnc;
    const char* raw = m.Str(sym.name);
    if (!SplitSymbol(raw, nid, libEnc)) {
        // Plain-named undefined symbols are weak references (module_start/stop and
        // the like) that the PS4 loader leaves null when nothing defines them.
        if (sym.Bind() == STB_WEAK) return 0;
        throw std::runtime_error(m.fileName + ": unresolvable symbol " + raw);
    }
    auto libIt = m.importLibs.find(DecodeId(libEnc));
    std::string lib = libIt != m.importLibs.end() ? libIt->second : libEnc;
    std::string key = nid + "#" + lib;

    if (auto it = exports_.find(key); it != exports_.end()) { ++stats_.fromModules; return it->second; }
    if (auto it = hle_.find(key); it != hle_.end()) {
        ++stats_.fromHle;
        if (CountCalls()) {
            if (auto c = countThunks_.find(key); c != countThunks_.end()) return c->second;
            return countThunks_[key] = CountingThunk(lib, Pretty(nid), it->second);
        }
        return it->second;
    }
    if (auto alias = aliases_.find(lib); alias != aliases_.end()) {
        const std::string aliasKey = nid + "#" + alias->second;
        if (auto it = exports_.find(aliasKey); it != exports_.end()) { ++stats_.fromModules; return it->second; }
        if (auto it = hle_.find(aliasKey); it != hle_.end()) { ++stats_.fromHle; return it->second; }
    }

    std::string name = Pretty(nid);
    if (reported_.insert(name + "#" + lib).second) stats_.missing[lib].push_back(name);

    if (sym.Type() == STT_OBJECT) {
        ++stats_.stubbedData;
        auto& storage = dataStubs_.emplace_back(kDataStubSize, 0);
        if (name == "__stack_chk_guard") {
            uint64_t guard = std::random_device{}() | (uint64_t{std::random_device{}()} << 32);
            std::memcpy(storage.data(), &guard, sizeof(guard));
        }
        return reinterpret_cast<uint64_t>(storage.data());
    }
    ++stats_.stubbedFuncs;
    return MakeStub(lib, name);
}

void Linker::Relocate(Module& m) {
    for (const auto& r : m.relas) {
        auto* where = reinterpret_cast<uint64_t*>(m.Addr(r.offset));
        const ElfSym* sym = r.Sym() < m.syms.size() ? &m.syms[r.Sym()] : nullptr;
        auto symAddr = [&]() -> uint64_t {
            if (!sym) return 0;
            if (sym->shndx != 0) return reinterpret_cast<uint64_t>(m.Addr(sym->value));
            return ResolveImport(m, *sym);
        };

        switch (r.Type()) {
        case R_X86_64_RELATIVE:
            *where = reinterpret_cast<uint64_t>(m.base) + r.addend;
            break;
        case R_X86_64_64:
            *where = symAddr() + r.addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = symAddr();
            break;
        case R_X86_64_DTPMOD64:
            *where = m.tls.index;
            break;
        case R_X86_64_DTPOFF64:
            *where = (sym ? sym->value : 0) + r.addend;
            break;
        case R_X86_64_TPOFF64:
            // TODO: TLS symbols imported from another module
            if (sym && sym->shndx == 0 && sym->Type() == STT_TLS)
                throw std::runtime_error(m.fileName + ": cross-module TPOFF64 not supported yet");
            *where = (sym ? sym->value : 0) + r.addend - m.tls.offset;
            break;
        default:
            throw std::runtime_error(m.fileName + ": unsupported relocation type " + std::to_string(r.Type()));
        }
        ++stats_.relocations;
    }
}

}  // namespace core
