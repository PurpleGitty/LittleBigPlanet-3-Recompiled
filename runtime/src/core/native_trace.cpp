// Call counts for the game script language's native functions (LBP3_COUNT_NATIVES=1).
//
// LBP3's scripts call engine functions through tables of {function, "Name__Signature"}
// pairs in the executable's data. Each function pointer is replaced with a small stub
// that counts the call and jumps on, so a report shows which natives a stuck script
// keeps calling (what it is waiting for). The pairs are found through the relocation
// table: a slot pointing at code followed by one pointing at a "...__..." name.
#include "core/native_trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/module.h"

namespace core {
namespace {

struct Native {
    const char* name;
    uint64_t function;  // module offset
    uint64_t* counter;
};

std::vector<Native> g_natives;

// lock inc qword [rip+8]; jmp qword [rip+10]; pad; counter; original
constexpr uint8_t kStub[16] = {0xF0, 0x48, 0xFF, 0x05, 0x08, 0x00, 0x00, 0x00,
                               0xFF, 0x25, 0x0A, 0x00, 0x00, 0x00, 0xCC, 0xCC};
constexpr size_t kStubSize = 32;

bool IsCode(const Module& m, uint64_t off) {
    for (auto [base, size] : m.CodeRanges()) {
        const uint64_t start = static_cast<uint64_t>(base - m.base);
        if (off >= start && off < start + size) return true;
    }
    return false;
}

bool IsNativeName(const Module& m, uint64_t off) {
    if (off >= m.size) return false;
    const char* s = reinterpret_cast<const char*>(m.base + off);
    const size_t n = strnlen(s, 128);
    if (n < 3 || n == 128) return false;
    for (size_t i = 0; i < n; ++i)
        if (s[i] < 0x20 || s[i] >= 0x7f) return false;
    return std::strstr(s, "__") != nullptr;
}

// LBP3_NATIVE_TIMELINE=1: once a second, logs natives called for the first time and
// natives that stopped being called (after running for 2+ seconds), with a timestamp, to
// show what the scripts do around an event (e.g. the end of a cutscene).
void TimelineThread() {
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint64_t> last(g_natives.size(), 0);
    std::vector<int> activeSecs(g_natives.size(), 0);
    std::vector<bool> seen(g_natives.size(), false);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (size_t i = 0; i < g_natives.size(); ++i) {
            const uint64_t v = *g_natives[i].counter;
            const uint64_t delta = v >= last[i] ? v - last[i] : v;  // PrintNativeCounts resets
            last[i] = v;
            if (delta && !seen[i]) {
                seen[i] = true;
                std::fprintf(stderr, "[natives] t=%.0f new %s x%llu\n", t, g_natives[i].name,
                             static_cast<unsigned long long>(delta));
            }
            if (delta) {
                ++activeSecs[i];
            } else {
                if (activeSecs[i] >= 2) std::fprintf(stderr, "[natives] t=%.0f stopped %s\n", t, g_natives[i].name);
                activeSecs[i] = 0;
            }
        }
    }
}

}  // namespace

void InstallNativeCounters(Module& m) {
    const bool dump = std::getenv("LBP3_DUMP_NATIVES") != nullptr;  // write analysis\natives.txt
    const bool timeline = std::getenv("LBP3_NATIVE_TIMELINE") != nullptr;
    if (!std::getenv("LBP3_COUNT_NATIVES") && !dump && !timeline) return;
    std::unordered_map<uint64_t, int64_t> slots;  // slot offset -> target offset
    for (const auto& r : m.relas) slots[r.offset] = r.addend;
    std::vector<uint64_t> found;
    for (const auto& [slot, target] : slots) {
        auto name = slots.find(slot + 8);
        if (name == slots.end() || !IsCode(m, static_cast<uint64_t>(target)) ||
            !IsNativeName(m, static_cast<uint64_t>(name->second)))
            continue;
        found.push_back(slot);
    }
    if (found.empty()) return;
    if (dump) {
        if (FILE* f = std::fopen("analysis\\natives.txt", "w")) {
            for (uint64_t slot : found)
                std::fprintf(f, "eboot+0x%llx  %s\n", static_cast<unsigned long long>(slots[slot]),
                             reinterpret_cast<const char*>(m.base + slots[slot + 8]));
            std::fclose(f);
        }
        if (!std::getenv("LBP3_COUNT_NATIVES") && !timeline) return;
    }
    auto* stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, found.size() * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stubs) return;
    for (size_t i = 0; i < found.size(); ++i) {
        auto* slot = reinterpret_cast<uint64_t*>(m.base + found[i]);
        uint8_t* stub = stubs + i * kStubSize;
        std::memcpy(stub, kStub, sizeof(kStub));
        auto* counter = reinterpret_cast<uint64_t*>(stub + 16);
        *counter = 0;
        *reinterpret_cast<uint64_t*>(stub + 24) = *slot;  // the relocated function address
        g_natives.push_back({reinterpret_cast<const char*>(m.base + slots[found[i] + 8]),
                             static_cast<uint64_t>(slots[found[i]]), counter});
        DWORD old;
        VirtualProtect(slot, 8, PAGE_READWRITE, &old);
        *slot = reinterpret_cast<uint64_t>(stub);
        VirtualProtect(slot, 8, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, found.size() * kStubSize);
    std::fprintf(stderr, "[natives] counting calls to %zu script natives\n", g_natives.size());
    if (timeline) std::thread(TimelineThread).detach();
}

ScriptNative HookNative(Module& m, const char* name, ScriptNative replacement) {
    std::unordered_map<uint64_t, int64_t> slots;
    for (const auto& r : m.relas) slots[r.offset] = r.addend;
    ScriptNative previous = nullptr;
    int patched = 0;
    for (const auto& [slot, target] : slots) {  // every table that binds this name
        auto nameSlot = slots.find(slot + 8);
        if (nameSlot == slots.end() || static_cast<uint64_t>(nameSlot->second) >= m.size) continue;
        if (std::strcmp(reinterpret_cast<const char*>(m.base + nameSlot->second), name) != 0) continue;
        if (!IsCode(m, static_cast<uint64_t>(target))) continue;
        auto* entry = reinterpret_cast<uint64_t*>(m.base + slot);
        if (!previous) previous = reinterpret_cast<ScriptNative>(*entry);  // may be a counting stub
        DWORD old;
        VirtualProtect(entry, 8, PAGE_READWRITE, &old);
        *entry = reinterpret_cast<uint64_t>(replacement);
        VirtualProtect(entry, 8, old, &old);
        ++patched;
    }
    std::fprintf(stderr, "[natives] %s: %d binding(s) hooked\n", name, patched);
    return previous;
}

void PrintNativeCounts(int top) {
    if (g_natives.empty()) return;
    std::vector<std::pair<uint64_t, const Native*>> v;
    for (auto& n : g_natives)
        if (*n.counter) v.emplace_back(*n.counter, &n);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[natives] most-called script natives since the last report:\n");
    for (size_t i = 0; i < v.size() && i < static_cast<size_t>(top); ++i)
        std::fprintf(stderr, "[natives] %10llu  %s (eboot+0x%llx)\n", static_cast<unsigned long long>(v[i].first),
                     v[i].second->name, static_cast<unsigned long long>(v[i].second->function));
    for (auto& n : g_natives) *n.counter = 0;
}

}  // namespace core
