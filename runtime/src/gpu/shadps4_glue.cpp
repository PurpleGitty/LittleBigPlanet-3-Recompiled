// The handful of shadPS4 functions the shader recompiler links against that
// live outside it (logging, asserts, user paths, signal handlers). Everything
// else is shadPS4's own code, built unmodified from third_party/shadPS4.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fmt/format.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/signals.h"

namespace Common::Log {

// Warnings and up by default; the recompiler is chatty at Info.
std::array<Level, NUM_LOG_CLASSES> g_class_levels = [] {
    std::array<Level, NUM_LOG_CLASSES> a{};
    a.fill(Level::Warning);
    return a;
}();

void VLog(Class, Level level, const char* file, int line, const char* func, fmt::string_view format,
          fmt::format_args args) {
    static const char* const kNames[] = {"trace", "debug", "info", "warning", "error", "critical", "off"};
    // Per call site: the first 20 messages, then every 1000th with a count. Some
    // warnings fire hundreds of times a frame, and the logging itself was slow.
    static std::mutex mutex;
    static std::map<std::pair<const char*, int>, uint64_t> counts;
    uint64_t n;
    {
        std::lock_guard lock(mutex);
        n = ++counts[{file, line}];
    }
    if (n > 20 && n % 1000 != 0) return;
    const char* slash = std::strrchr(file, '\\');
    if (!slash) slash = std::strrchr(file, '/');
    std::fprintf(stderr, "[shader %s] %s:%d %s: %s%s\n", kNames[static_cast<int>(level)], slash ? slash + 1 : file,
                 line, func, fmt::vformat(format, args).c_str(),
                 n > 20 ? fmt::format(" (x{})", n).c_str() : "");
}

}  // namespace Common::Log

void assert_fail_impl() {
    std::fflush(stderr);
    __debugbreak();  // the runtime's crash handler reports where
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    std::fprintf(stderr, "[shader critical] Assertion Failed!\n%s\n", msg);
    assert_fail_impl();
}

namespace Common::FS {

const std::filesystem::path& GetUserPath(PathType type) {
    // The pipeline cache lives with the other per-user data; everything else
    // shadPS4 writes (shader dumps) goes to analysis/.
    static const std::filesystem::path cache = std::filesystem::current_path() / "user" / "shader_cache";
    static const std::filesystem::path path = std::filesystem::current_path() / "analysis" / "shaders";
    return type == PathType::CacheDir ? cache : path;
}

std::string GetUserPathString(PathType type) { return GetUserPath(type).string(); }

}  // namespace Common::FS

namespace Core {

// shadPS4's Emulator fills ElfInfo (a friend of it) from param.sfo; we only
// need what the pipeline cache uses: the serial names its folder.
class Emulator {
public:
    static void InitElfInfo(const char* serial, u32 sdkVersion) {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = serial;
        info.sdk_ver = sdkVersion;
        info.firmware_ver = sdkVersion;
        info.raw_firmware_ver = sdkVersion;
        info.initialized = true;
    }
};

void InitElfInfo(const char* serial, u32 sdkVersion) { Emulator::InitElfInfo(serial, sdkVersion); }

namespace {

std::mutex g_mutex;
std::vector<AccessViolationHandler> g_handlers;

LONG CALLBACK Dispatch(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    void* fault = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]);
    // Copy the list and run the handlers unlocked: a handler can wait for the
    // GPU thread (readbacks), which may fault and land here at the same time.
    std::vector<AccessViolationHandler> handlers;
    {
        std::lock_guard lock(g_mutex);
        handlers = g_handlers;
    }
    for (auto h : handlers)
        if (h(ep, fault)) return EXCEPTION_CONTINUE_EXECUTION;  // handlers take EXCEPTION_POINTERS*
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

Signals* Signals::Instance() {
    static Signals s;
    return &s;
}

void Signals::RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32) {
    std::lock_guard lock(g_mutex);
    if (g_handlers.empty()) AddVectoredExceptionHandler(1, Dispatch);
    g_handlers.push_back(handler);
}

}  // namespace Core
