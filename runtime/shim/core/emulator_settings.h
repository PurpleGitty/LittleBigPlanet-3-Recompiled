// Stand-in for shadPS4's core/emulator_settings.h, covering what the shader
// recompiler and video_core read. Values are shadPS4's defaults unless noted.
#pragma once
#include <cstdlib>
#include <string>

#include "common/types.h"

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

struct ShimEmulatorSettings {
    // GPU
    bool IsNullGPU() const { return false; }
    bool IsCopyGpuBuffers() const { return false; }
    // LBP3_READBACKS=relaxed|precise copies GPU-written memory back for the CPU. Off by
    // default: turning it on didn't fix the Bunkum intro freeze and slowed loading.
    u32 GetReadbacksMode() const {
        const char* v = std::getenv("LBP3_READBACKS");
        if (!v) return GpuReadbacksMode::Disabled;
        return std::string(v) == "precise" ? GpuReadbacksMode::Precise : GpuReadbacksMode::Relaxed;
    }
    bool IsReadbackLinearImagesEnabled() const { return std::getenv("LBP3_READBACK_IMAGES") != nullptr; }
    bool IsDirectMemoryAccessEnabled() const { return false; }
    bool IsDumpShaders() const { return std::getenv("LBP3_DUMP_SHADERS") != nullptr; }  // analysis/shaders/
    bool IsPatchShaders() const { return false; }
    bool IsShaderCollect() const { return false; }
    bool IsInlineFetchShader() const { return false; }
    std::string GetPresentMode() const { return "Mailbox"; }
    bool IsHdrAllowed() const { return false; }
    bool IsFsrEnabled() const { return false; }
    bool IsRcasEnabled() const { return true; }
    int GetRcasAttenuation() const { return 250; }
    bool IsUserfaultfdTracking() const { return false; }

    // Vulkan
    s32 GetGpuId() const { return -1; }  // pick automatically
    bool IsRenderdocEnabled() const { return false; }
    bool IsVkValidationEnabled() const { return false; }
    bool IsVkValidationCoreEnabled() const { return true; }
    bool IsVkValidationSyncEnabled() const { return false; }
    bool IsVkValidationGpuEnabled() const { return false; }
    bool IsVkCrashDiagnosticEnabled() const { return false; }
    bool IsVkHostMarkersEnabled() const { return false; }
    bool IsVkGuestMarkersEnabled() const { return false; }
    // Compiled pipelines are kept in user/shader_cache/, so each shader compiles once
    // (first-sight compiles were the stutters). LBP3_NO_SHADER_CACHE=1 turns it off.
    bool IsPipelineCacheEnabled() const { return std::getenv("LBP3_NO_SHADER_CACHE") == nullptr; }
    bool IsPipelineCacheArchived() const { return false; }
};

inline ShimEmulatorSettings EmulatorSettings;
