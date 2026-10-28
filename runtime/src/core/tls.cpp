#include "core/tls.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <stdexcept>

namespace core {
namespace {

// TEB::TlsSlots; slots 0-63 live inline in the TEB at this gs offset.
constexpr uint32_t kTebTlsSlots = 0x1480;
constexpr DWORD kInlineSlots = 64;

DWORD g_slot = TLS_OUT_OF_INDEXES;
std::vector<Module*> g_modules;
uint64_t g_staticSize = 0;
uint64_t g_align = 16;

struct Block { uint8_t* base; Tcb* tcb; };
thread_local Block t_block{};

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

}  // namespace

void InitTls(const std::vector<Module*>& modules) {
    g_slot = TlsAlloc();
    if (g_slot == TLS_OUT_OF_INDEXES || g_slot >= kInlineSlots)
        throw std::runtime_error("no inline TEB TLS slot available");
    g_modules = modules;
    for (Module* m : modules) {
        if (!m->tls.index) continue;
        g_staticSize = std::max(g_staticSize, m->tls.offset);
        g_align = std::max(g_align, m->tls.align);
    }
    g_staticSize = AlignUp(g_staticSize, g_align);
}

size_t PatchFsAccesses(Module& m) {
    // mov r64, fs:[0]  =  64 REX.W(48..4F) 8B ModRM(00 reg 100) SIB(25) disp32(0)
    const uint32_t disp = kTebTlsSlots + g_slot * 8;
    size_t patched = 0;
    for (auto [code, size] : m.CodeRanges()) {
        for (size_t i = 0; i + 9 <= size; ++i) {
            uint8_t* p = code + i;
            if (p[0] != 0x64 || (p[1] & 0xF8) != 0x48 || p[2] != 0x8B || (p[3] & 0xC7) != 0x04 ||
                p[4] != 0x25 || p[5] | p[6] | p[7] | p[8])
                continue;
            p[0] = 0x65;  // gs
            std::memcpy(p + 5, &disp, 4);
            ++patched;
            i += 8;
        }
    }
    return patched;
}

Tcb* SetupThreadTls() {
    const size_t total = g_staticSize + sizeof(Tcb);
    auto* base = static_cast<uint8_t*>(_aligned_malloc(total, g_align));
    if (!base) throw std::runtime_error("out of memory for TLS");
    std::memset(base, 0, total);
    auto* tcb = reinterpret_cast<Tcb*>(base + g_staticSize);
    tcb->self = tcb;

    // Variant II: each module's block sits `offset` bytes below the TCB.
    for (Module* m : g_modules) {
        if (!m->tls.index) continue;
        uint8_t* dst = reinterpret_cast<uint8_t*>(tcb) - m->tls.offset;
        std::memcpy(dst, m->Addr(m->tls.vaddr), m->tls.fileSize);
    }
    t_block = {base, tcb};
    TlsSetValue(g_slot, tcb);
    return tcb;
}

void FreeThreadTls() {
    TlsSetValue(g_slot, nullptr);
    _aligned_free(t_block.base);
    t_block = {};
}

Tcb* CurrentTcb() { return static_cast<Tcb*>(TlsGetValue(g_slot)); }

void* TlsAddress(uint64_t moduleIndex, uint64_t offset) {
    auto* tcb = static_cast<uint8_t*>(TlsGetValue(g_slot));
    for (Module* m : g_modules)
        if (m->tls.index == moduleIndex) return tcb - m->tls.offset + offset;
    return nullptr;
}

}  // namespace core
