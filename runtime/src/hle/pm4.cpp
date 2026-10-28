#include "hle/pm4.h"

#include "gpu/shader_translate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>

namespace hle::pm4 {
namespace {

enum Opcode : uint32_t {
    kDispatchDirect = 0x15,
    kDispatchIndirect = 0x16,
    kDrawIndexIndirect = 0x25,
    kDrawIndex2 = 0x27,
    kDrawIndexAuto = 0x2D,
    kDrawIndexOffset2 = 0x35,
    kSetContextReg = 0x69,
    kSetShReg = 0x76,
    kWriteData = 0x37,
    kIndirectBuffer = 0x3F,
    kEventWriteEop = 0x47,
    kEventWriteEos = 0x48,
    kReleaseMem = 0x49,
};

enum DataSelect : uint32_t { kNone = 0, kData32 = 1, kData64 = 2, kGpuClock64 = 3, kPerfCounter = 4 };

constexpr int kMaxIndirectDepth = 4;
constexpr uint32_t kContextRegCount = 0x400;
constexpr uint32_t kCbColor0Base = 0x318;   // context register index
constexpr uint32_t kCbColor0Pitch = 0x319;

constexpr uint32_t kShRegCount = 0x400;
constexpr uint32_t kSpiShaderPgmLoPs = 0x8;
constexpr uint32_t kSpiShaderPgmLoVs = 0x48;

// Register shadows, persisted across submits like on the GPU.
uint32_t g_ctx[kContextRegCount];
uint32_t g_sh[kShRegCount];

uint64_t GpuClock() {
    using namespace std::chrono;
    // The GPU clock ticks at 100 MHz on PS4.
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 10);
}

std::atomic<int> g_logged{0};
constexpr int kLogFirstWrites = 0;

void LogWrite(const char* what, uint64_t addr, uint64_t value) {
    if (g_logged++ < kLogFirstWrites)
        std::fprintf(stderr, "[pm4] %s -> %p = 0x%llx\n", what, reinterpret_cast<void*>(addr),
                     static_cast<unsigned long long>(value));
}

void WriteFence(uint64_t addr, uint32_t dataSel, uint32_t lo, uint32_t hi, Result& r) {
    if (!addr) return;
    LogWrite("fence", addr, lo | uint64_t{hi} << 32);
    switch (dataSel) {
    case kNone: return;
    case kData32: std::memcpy(reinterpret_cast<void*>(addr), &lo, 4); break;
    case kData64: {
        const uint64_t v = lo | uint64_t{hi} << 32;
        std::memcpy(reinterpret_cast<void*>(addr), &v, 8);
        break;
    }
    case kGpuClock64:
    case kPerfCounter: {
        const uint64_t v = GpuClock();
        std::memcpy(reinterpret_cast<void*>(addr), &v, 8);
        break;
    }
    default: return;
    }
    ++r.labelWrites;
}

// Saves each new shader binary to analysis/shaders/<stage>_<address>.bin for
// the future shader recompiler. GCN programs end with s_endpgm (0xBF810000);
// a header-less scan for it bounds the copy.
void NoteShader(const char* stage, uint64_t addr) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    constexpr size_t kMaxShaderBytes = 64 * 1024;
    constexpr uint32_t kSEndPgm = 0xBF810000;
    if (!addr) return;
    std::lock_guard lock(m);
    if (!seen.insert(addr).second) return;
    const auto* code = reinterpret_cast<const uint32_t*>(addr);
    size_t n = 0;
    while (n < kMaxShaderBytes / 4 && code[n] != kSEndPgm) ++n;
    std::filesystem::create_directories("analysis/shaders");
    char name[96];
    std::snprintf(name, sizeof(name), "analysis/shaders/%s_%llx.bin", stage, static_cast<unsigned long long>(addr));
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(code, 4, n + 1, f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[pm4] new %s shader at %p (%zu bytes)\n", stage, reinterpret_cast<void*>(addr), (n + 1) * 4);
}

void Walk(const uint32_t* p, size_t dwords, int depth, Result& r) {
    size_t i = 0;
    while (i < dwords) {
        const uint32_t header = p[i];
        const uint32_t type = header >> 30;
        if (type == 2) { ++i; continue; }          // filler
        if (type == 0) {                             // register writes
            i += ((header >> 16) & 0x3FFF) + 2;
            continue;
        }
        if (type != 3) {
            std::fprintf(stderr, "[pm4] type-1 header 0x%08x at dword %zu of %zu\n", header, i, dwords);
            ++i;
            continue;
        }
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;  // body dwords
        const uint32_t op = (header >> 8) & 0xFF;
        const uint32_t* b = p + i + 1;
        if (i + 1 + count > dwords) {
            std::fprintf(stderr, "[pm4] truncated packet op 0x%02x count %u at dword %zu of %zu (depth %d)\n", op,
                         count, i, dwords, depth);
            break;
        }

        switch (op) {
        case kEventWriteEop: {
            const uint64_t addr = b[1] | uint64_t{b[2] & 0xFFFF} << 32;
            WriteFence(addr, b[2] >> 29, b[3], b[4], r);
            if ((b[2] >> 24) & 3) r.interrupt = true;
            break;
        }
        case kReleaseMem: {
            const uint64_t addr = b[2] | uint64_t{b[3]} << 32;
            WriteFence(addr, b[1] >> 29, b[4], b[5], r);
            if ((b[1] >> 24) & 7) r.interrupt = true;
            break;
        }
        case kEventWriteEos: {
            const uint64_t addr = b[1] | uint64_t{b[2] & 0xFFFF} << 32;
            if ((b[2] >> 29) == 2) WriteFence(addr, kData32, b[3], 0, r);  // store data
            break;
        }
        case kWriteData: {
            const uint32_t dstSel = (b[0] >> 8) & 0xF;
            const uint64_t addr = b[1] | uint64_t{b[2]} << 32;
            // dst_sel 1/5 = memory; register and GDS writes don't matter yet.
            if ((dstSel == 1 || dstSel == 5) && addr && count > 3) {
                std::memcpy(reinterpret_cast<void*>(addr), b + 3, (count - 3) * 4);
                LogWrite("write_data", addr, b[3]);
                ++r.labelWrites;
            }
            break;
        }
        case kSetContextReg: {
            const uint32_t reg = b[0] & 0xFFFF;
            for (uint32_t k = 0; k + 1 < count && reg + k < kContextRegCount; ++k) g_ctx[reg + k] = b[1 + k];
            break;
        }
        case kSetShReg: {
            const uint32_t reg = b[0] & 0xFFFF;
            for (uint32_t k = 0; k + 1 < count && reg + k < kShRegCount; ++k) g_sh[reg + k] = b[1 + k];
            break;
        }
        case kDrawIndex2:
        case kDrawIndexAuto:
        case kDrawIndexOffset2:
        case kDrawIndexIndirect:
            ++r.draws;
            NoteShader("ps", uint64_t{g_sh[kSpiShaderPgmLoPs]} << 8);
            gpu::TranslateBoundPixelShader(g_sh, g_ctx);
            NoteShader("vs", uint64_t{g_sh[kSpiShaderPgmLoVs]} << 8);
            r.lastColorTarget = uint64_t{g_ctx[kCbColor0Base]} << 8;
            r.lastTargetPitch = g_ctx[kCbColor0Pitch];
            break;
        case kDispatchDirect:
        case kDispatchIndirect:
            ++r.dispatches;
            break;
        case kIndirectBuffer: {
            const uint64_t addr = (b[0] & ~3u) | uint64_t{b[1] & 0xFFFF} << 32;
            const uint32_t size = b[2] & 0xFFFFF;  // in dwords
            if (addr && depth < kMaxIndirectDepth)
                Walk(reinterpret_cast<const uint32_t*>(addr), size, depth + 1, r);
            break;
        }
        default: break;
        }
        i += 1 + count;
    }
}

}  // namespace

long FindOverrun(const uint32_t* p, size_t dwords) {
    size_t i = 0;
    while (i < dwords) {
        const uint32_t type = p[i] >> 30;
        size_t len = type == 3 ? ((p[i] >> 16) & 0x3FFF) + 2 : type == 0 ? ((p[i] >> 16) & 0x3FFF) + 2 : 1;
        if (i + len > dwords) return static_cast<long>(i);
        i += len;
    }
    return -1;
}

Result Execute(const uint32_t* cmds, size_t sizeBytes) {
    Result r;
    if (cmds) Walk(cmds, sizeBytes / 4, 0, r);
    return r;
}

}  // namespace hle::pm4
