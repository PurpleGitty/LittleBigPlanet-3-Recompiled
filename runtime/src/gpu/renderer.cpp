// Wiring follows shadPS4's Presenter constructor and gnmdriver PerformSubmit
// (GPL-2.0-or-later).
#include "gpu/renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "common/elf_info.h"
#include "core/emulator_settings.h"
#include "core/libraries/gnmdriver/gnm_error.h"
#include "core/libraries/gnmdriver/gnmdriver_init.h"
#include "core/platform.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "core/libraries/videoout/buffer.h"

#include <vk_mem_alloc.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace core {
void* ProcParam();  // runtime/src/core/runner.h (not included: shadPS4 has its own core/ headers)
}

namespace Core {
void ClearCommittedRangeCache();  // video_core_glue.cpp
void InitElfInfo(const char* serial, u32 sdkVersion);  // shadps4_glue.cpp
}

namespace gpu {
namespace {

// These live for the whole process and are deliberately never destroyed:
// tearing them down during static destruction runs after Vulkan's dispatch
// tables are gone.
Frontend::WindowSDL g_headless;  // reports no surface: device without a swapchain
Vulkan::Instance* g_instance = nullptr;
AmdGpu::Liverpool* g_liverpool = nullptr;
Vulkan::Scheduler* g_scheduler = nullptr;
Vulkan::Runtime* g_runtime = nullptr;
Vulkan::Rasterizer* g_rasterizer = nullptr;

std::mutex g_handlerMutex;
std::function<void(uint32_t)> g_handler;
bool g_sentInit = false;

// Game mappings made before the renderer exists, replayed by Init.
std::mutex g_mapMutex;
std::map<uint64_t, uint64_t> g_pendingMaps;
std::map<uint64_t, uint64_t> g_allMaps;  // every live mapping (for the render target dump)

void RegisterWithRasterizer(uint64_t addr, uint64_t size) {
    g_rasterizer->RegisterMemory(addr, size);
    g_rasterizer->MapMemory(addr, size);
}

// The interrupts a game can register for through sceGnmAddEqEvent.
constexpr Platform::InterruptId kForwardedIrqs[] = {Platform::InterruptId::GfxEop, Platform::InterruptId::GfxFlip};

uint32_t SdkVersion() {
    struct ProcParam { uint64_t size; uint32_t magic, entryCount; uint64_t sdkVersion; };
    const auto* p = static_cast<const ProcParam*>(core::ProcParam());
    return p ? static_cast<uint32_t>(p->sdkVersion) : Common::ElfInfo::FW_200;
}

}  // namespace

bool Init() {
    if (g_instance) return true;
    g_instance = new Vulkan::Instance(g_headless, EmulatorSettings.GetGpuId(),
                                      EmulatorSettings.IsVkValidationEnabled(),
                                      EmulatorSettings.IsVkCrashDiagnosticEnabled());
    std::fprintf(stderr, "[gpu] Vulkan device: %s (driver %s)\n", std::string(g_instance->GetModelName()).c_str(),
                 g_instance->GetDriverVersionName().c_str());

    Core::InitElfInfo("CUSA00473", SdkVersion());  // the pipeline cache keys its folder on the serial
    g_liverpool = new AmdGpu::Liverpool();
    g_scheduler = new Vulkan::Scheduler(*g_instance);
    g_runtime = new Vulkan::Runtime(*g_instance, *g_scheduler);
    {
        std::lock_guard lock(g_mapMutex);
        g_rasterizer = new Vulkan::Rasterizer(*g_instance, *g_scheduler, *g_runtime, g_liverpool);
        for (auto [addr, size] : g_pendingMaps) RegisterWithRasterizer(addr, size);
        g_pendingMaps.clear();
    }

    for (auto irq : kForwardedIrqs) {
        Platform::IrqC::Instance()->Register(
            irq,
            [](Platform::InterruptId id) {
                std::lock_guard lock(g_handlerMutex);
                if (g_handler) g_handler(static_cast<uint32_t>(id));
            },
            &g_handler);
    }
    std::fprintf(stderr, "[gpu] command processor and rasterizer ready\n");
    return true;
}

bool Active() { return g_rasterizer != nullptr; }

void MapMemory(uint64_t addr, uint64_t size) {
    Core::ClearCommittedRangeCache();
    constexpr uint64_t kMaxGpuAddress = 1ull << 40;  // the PS4 GPU sees 40-bit addresses
    if (addr + size > kMaxGpuAddress) return;
    std::lock_guard lock(g_mapMutex);
    g_allMaps[addr] = size;
    if (g_rasterizer) RegisterWithRasterizer(addr, size);
    else g_pendingMaps[addr] = size;
}

void UnmapMemory(uint64_t addr, uint64_t size) {
    Core::ClearCommittedRangeCache();
    std::lock_guard lock(g_mapMutex);
    g_allMaps.erase(addr);
    if (g_rasterizer) g_rasterizer->UnmapMemory(addr, size);
    else g_pendingMaps.erase(addr);
}

void SetInterruptHandler(std::function<void(uint32_t)> handler) {
    std::lock_guard lock(g_handlerMutex);
    g_handler = std::move(handler);
}

namespace {

// Host-visible buffer the display image is copied into each flip.
struct Staging {
    vk::Buffer buffer{};
    VmaAllocation allocation{};
    void* mapped = nullptr;
    vk::DeviceSize size = 0;
} g_staging;

bool EnsureStaging(vk::DeviceSize size) {
    if (g_staging.size >= size) return true;
    if (g_staging.buffer) vmaDestroyBuffer(g_instance->GetAllocator(), g_staging.buffer, g_staging.allocation);
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buf{};
    VmaAllocationInfo out{};
    if (vmaCreateBuffer(g_instance->GetAllocator(), &info, &alloc, &buf, &g_staging.allocation, &out) != VK_SUCCESS)
        return false;
    g_staging.buffer = buf;
    g_staging.mapped = out.pMappedData;
    g_staging.size = size;
    return true;
}

}  // namespace

bool ReadbackDisplayBuffer(const DisplayBufferDesc& d, std::vector<uint8_t>& out, uint32_t& width,
                           uint32_t& height) {
    if (!g_rasterizer || !d.address) return false;
    Libraries::VideoOut::BufferAttributeGroup group{};
    group.is_occupied = true;
    group.attrib.pixel_format = static_cast<Libraries::VideoOut::PixelFormat>(d.pixelFormat);
    group.attrib.tiling_mode = static_cast<Libraries::VideoOut::TilingMode>(d.tilingMode);
    group.attrib.width = d.width;
    group.attrib.height = d.height;
    group.attrib.pitch_in_pixel = d.pitchPixels;

    auto& cache = g_rasterizer->GetTextureCache();
    VideoCore::TextureCache::ImageDesc desc{group, d.address};  // FindImage takes it by mutable reference
    const auto id = cache.FindImage(desc);
    cache.UpdateImage(id);
    auto& image = cache.GetImage(id);
    width = image.info.size.width;
    height = image.info.size.height;
    const vk::DeviceSize bytes = vk::DeviceSize{width} * height * 4;
    if (!EnsureStaging(bytes)) return false;

    g_scheduler->EndRendering();
    const auto cmd = g_scheduler->CommandBuffer();
    VideoCore::Image::Barriers barriers;
    image.GetBarriers(barriers, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      vk::PipelineStageFlagBits2::eTransfer);
    if (!barriers.empty()) {
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                                .pImageMemoryBarriers = barriers.data()});
    }
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    cmd.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, g_staging.buffer, region);
    g_scheduler->Finish();  // wait for the GPU; fine for a first version

    vmaInvalidateAllocation(g_instance->GetAllocator(), g_staging.allocation, 0, bytes);
    out.resize(bytes);
    std::memcpy(out.data(), g_staging.mapped, bytes);
    return true;
}

namespace {

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    const float s = sign ? -1.f : 1.f;
    if (exp == 0) return s * std::ldexp(static_cast<float>(man), -24);
    if (exp == 31) return man ? 0.f : s * 65504.f;
    return s * std::ldexp(1.f + man / 1024.f, static_cast<int>(exp) - 15);
}

// 11/10-bit unsigned floats (5-bit exponent, no sign).
float SmallFloat(uint32_t bits, int manBits) {
    const uint32_t exp = bits >> manBits, man = bits & ((1u << manBits) - 1);
    if (exp == 0) return std::ldexp(static_cast<float>(man), -14 - manBits);
    if (exp == 31) return 65504.f;
    return std::ldexp(1.f + static_cast<float>(man) / (1u << manBits), static_cast<int>(exp) - 15);
}

uint8_t ToByte(float v) {  // linear HDR value -> 0..255 with a simple Reinhard curve
    if (!(v > 0.f)) return 0;
    v = v / (1.f + v);
    return static_cast<uint8_t>(std::min(255.f, std::pow(v, 1.f / 2.2f) * 255.f + 0.5f));
}

// Decodes one pixel to rgb (0..255); returns bytes per pixel, 0 if the format is unsupported.
uint32_t DecodePixel(vk::Format f, const uint8_t* p, uint8_t rgb[3]) {
    uint32_t v = 0;
    uint16_t h[4]{};
    switch (f) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
        return 4;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
        return 4;
    case vk::Format::eA2B10G10R10UnormPack32:
        std::memcpy(&v, p, 4);
        rgb[0] = static_cast<uint8_t>((v & 0x3ff) >> 2);
        rgb[1] = static_cast<uint8_t>(((v >> 10) & 0x3ff) >> 2);
        rgb[2] = static_cast<uint8_t>(((v >> 20) & 0x3ff) >> 2);
        return 4;
    case vk::Format::eA2R10G10B10UnormPack32:
        std::memcpy(&v, p, 4);
        rgb[2] = static_cast<uint8_t>((v & 0x3ff) >> 2);
        rgb[1] = static_cast<uint8_t>(((v >> 10) & 0x3ff) >> 2);
        rgb[0] = static_cast<uint8_t>(((v >> 20) & 0x3ff) >> 2);
        return 4;
    case vk::Format::eB10G11R11UfloatPack32:
        std::memcpy(&v, p, 4);
        rgb[0] = ToByte(SmallFloat(v & 0x7ff, 6));
        rgb[1] = ToByte(SmallFloat((v >> 11) & 0x7ff, 6));
        rgb[2] = ToByte(SmallFloat(v >> 22, 5));
        return 4;
    case vk::Format::eR16G16B16A16Sfloat:
        std::memcpy(h, p, 8);
        for (int i = 0; i < 3; ++i) rgb[i] = ToByte(HalfToFloat(h[i]));
        return 8;
    case vk::Format::eR16G16Sfloat:
        std::memcpy(h, p, 4);
        rgb[0] = ToByte(HalfToFloat(h[0]));
        rgb[1] = ToByte(HalfToFloat(h[1]));
        rgb[2] = 0;
        return 4;
    case vk::Format::eR16G16B16A16Unorm:
        rgb[0] = p[1]; rgb[1] = p[3]; rgb[2] = p[5];
        return 8;
    case vk::Format::eR32Sfloat: {
        float fv;
        std::memcpy(&fv, p, 4);
        rgb[0] = rgb[1] = rgb[2] = static_cast<uint8_t>(std::clamp(fv, 0.f, 1.f) * 255.f);
        return 4;
    }
    case vk::Format::eR16Sfloat:
        std::memcpy(h, p, 2);
        rgb[0] = rgb[1] = rgb[2] = ToByte(HalfToFloat(h[0]));
        return 2;
    case vk::Format::eR8Unorm:
        rgb[0] = rgb[1] = rgb[2] = p[0];
        return 1;
    case vk::Format::eR8G8Unorm:
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = 0;
        return 2;
    default:
        return 0;
    }
}

uint32_t FormatBytes(vk::Format f) {
    static const uint8_t zero[16]{};
    uint8_t rgb[3];
    return DecodePixel(f, zero, rgb);
}

void WriteBmp(const char* name, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
    const uint32_t row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(54 + size_t{row} * h);
    auto put32 = [&](size_t at, uint32_t val) { std::memcpy(&out[at], &val, 4); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1;
    out[28] = 24;
    put32(34, row * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = &rgb[(size_t{h - 1 - y} * w + x) * 3];  // BMP rows go bottom-up
            uint8_t* d = &out[54 + size_t{y} * row + x * 3];
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

}  // namespace

// LBP3_DUMP_TARGETS=<flip number>: at that flip, write every render target the
// texture cache holds to analysis/targets/ (one BMP each; the log lists size,
// format, guest address, last-use tick and average brightness).
void DumpImage(VideoCore::Image& image, const char* base);

// LBP3_DUMP_DRAWS=<n>[,<n>...]: while a frame is traced, save render target 0
// right after draw #n (the numbering of the [draw] log lines) to analysis/targets/draws/.
void AfterTracedDraw(uint32_t index) {
    // The list comes from LBP3_DUMP_DRAWS or, re-read at every traced frame, from
    // analysis\dump_draws.txt (so draws can be picked after looking at a trace).
    static std::vector<uint32_t> wanted;
    if (index == 1) {
        wanted.clear();
        std::string spec;
        if (const char* v = std::getenv("LBP3_DUMP_DRAWS")) spec = v;
        if (FILE* f = std::fopen("analysis\\dump_draws.txt", "r")) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) spec += std::string(",") + buf;
            std::fclose(f);
        }
        for (const char* p = spec.c_str(); *p;) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(p, &end, 10);
            if (end == p) {
                ++p;
                continue;
            }
            wanted.push_back(static_cast<uint32_t>(n));
            p = end;
        }
    }
    if (std::find(wanted.begin(), wanted.end(), index) == wanted.end() || !g_rasterizer) return;
    auto& cache = g_rasterizer->GetTextureCache();
    CreateDirectoryA("analysis\\targets", nullptr);
    CreateDirectoryA("analysis\\targets\\draws", nullptr);
    // The depth buffer too, and every depth image at the addresses the draw samples.
    const auto& db = g_liverpool->regs.depth_buffer;
    if (db.DepthValid()) {
        cache.ForEachImageWithAddress(db.DepthAddress(), [&](VideoCore::ImageId, VideoCore::Image& image) {
            if (!image.usage.depth_target) return;
            char base[96];
            std::snprintf(base, sizeof(base), "analysis\\targets\\draws\\draw%04u_z", index);
            DumpImage(image, base);
        });
    }
    for (uint32_t i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
        const auto& cb = g_liverpool->regs.color_buffers[i];
        if (!cb) continue;
        // Every render-target image at the address: the pool there is aliased by several.
        int n = 0;
        cache.ForEachImageWithAddress(cb.Address(), [&](VideoCore::ImageId, VideoCore::Image& image) {
            if (!image.usage.render_target) return;
            char base[96];
            std::snprintf(base, sizeof(base), "analysis\\targets\\draws\\draw%04u_rt%u_%d", index, i, n++);
            DumpImage(image, base);
        });
    }
}

std::atomic<bool> g_dumpRequested{false};

void RequestTargetDump() { g_dumpRequested = true; }

void MaybeDumpRenderTargets() {
    static int target = [] {
        const char* v = std::getenv("LBP3_DUMP_TARGETS");
        return v ? std::atoi(v) : -1;
    }();
    static int flips = 0;
    static int dumps = 0;
    // The F9 hotkey: trace the next frame, dump at the flip after it.
    static bool hotkeyDump = false;
    if (g_dumpRequested.exchange(false) && !hotkeyDump) {
        hotkeyDump = true;
        target = flips + 1;
    }
    // Trace every draw of the frame before the dump (LBP3_TRACE_DRAWS=1, always for F9).
    static const bool traceDraws = std::getenv("LBP3_TRACE_DRAWS") != nullptr;
    if ((traceDraws || hotkeyDump) && target >= 0) {
        if (flips == target - 1) {
            std::fprintf(stderr, "[draw] ---- frame %d begins ----\n", target);
            Vulkan::g_lbp3TraceIndex = 0;
            Vulkan::g_lbp3AfterDraw = AfterTracedDraw;
            Vulkan::g_lbp3TraceDraws = true;
        } else if (flips == target) {
            Vulkan::g_lbp3TraceDraws = false;
        }
    }
    if (target < 0 || flips++ != target || !g_rasterizer) return;
    hotkeyDump = false;
    char dir[64];
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::snprintf(dir, sizeof(dir), "analysis\\targets\\dump_%02d%02d%02d_%d", now.wHour, now.wMinute, now.wSecond,
                  ++dumps);

    auto& cache = g_rasterizer->GetTextureCache();
    std::vector<VideoCore::ImageId> ids;
    {
        std::lock_guard lock(g_mapMutex);
        for (auto [addr, size] : g_allMaps)
            cache.ForEachImageInRegion(addr, size, [&](VideoCore::ImageId id, VideoCore::Image& image) {
                if ((image.usage.render_target || image.usage.depth_target) &&
                    std::find(ids.begin(), ids.end(), id) == ids.end())
                    ids.push_back(id);
            });
    }
    CreateDirectoryA("analysis\\targets", nullptr);
    CreateDirectoryA(dir, nullptr);
    std::fprintf(stderr, "[targets] %zu render targets at flip %d into %s (current tick %llu)\n", ids.size(),
                 target, dir, static_cast<unsigned long long>(g_scheduler->CurrentTick()));
    int index = 0;
    for (const auto id : ids) {
        auto& image = cache.GetImage(id);
        char base[96];
        std::snprintf(base, sizeof(base), "%s\\t%02d", dir, index++);
        DumpImage(image, base);
    }
    std::fflush(stderr);
}

// Copies one image out of the GPU (stalls until done) and writes <base>_WxH_addr.bmp
// plus an _alpha.bmp for 8-bit RGBA formats, with a summary line in the log.
void DumpImage(VideoCore::Image& image, const char* base) {
    {
        const vk::Format fmt = image.info.pixel_format;
        // Depth: the depth aspect copies out as 32-bit floats (D32 formats).
        const bool depth = fmt == vk::Format::eD32Sfloat || fmt == vk::Format::eD32SfloatS8Uint;
        const uint32_t bpp = depth ? 4 : FormatBytes(fmt);
        const uint32_t w = image.info.size.width, h = image.info.size.height;
        std::fprintf(stderr, "[targets] %s %ux%u %s at %llx, last used tick %llu", base, w, h,
                     vk::to_string(fmt).c_str(), static_cast<unsigned long long>(image.info.guest_address),
                     static_cast<unsigned long long>(image.tick_accessed_last));
        if (!bpp || w == 0 || h == 0 || w > 8192 || h > 8192) {
            std::fprintf(stderr, " (not dumped)\n");
            return;
        }
        const vk::DeviceSize bytes = vk::DeviceSize{w} * h * bpp;
        if (!EnsureStaging(bytes)) return;
        g_scheduler->EndRendering();
        const auto cmd = g_scheduler->CommandBuffer();
        VideoCore::Image::Barriers barriers;
        image.GetBarriers(barriers, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                          vk::PipelineStageFlagBits2::eTransfer);
        if (!barriers.empty())
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                                    .pImageMemoryBarriers = barriers.data()});
        const auto aspect = depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
        const vk::BufferImageCopy region{.imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {w, h, 1}};
        cmd.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, g_staging.buffer, region);
        g_scheduler->Finish();
        vmaInvalidateAllocation(g_instance->GetAllocator(), g_staging.allocation, 0, bytes);
        const auto* src = static_cast<const uint8_t*>(g_staging.mapped);
        if (std::getenv("LBP3_DUMP_RAW")) {  // the untouched texels too, for exact values
            char name[128];
            std::snprintf(name, sizeof(name), "%s_%ux%u_%llx.raw", base, w, h,
                          static_cast<unsigned long long>(image.info.guest_address));
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(src, 1, bytes, f);
                std::fclose(f);
            }
        }
        std::vector<uint8_t> rgb(size_t{w} * h * 3);
        if (depth) {  // log the value range, write the image stretched to it
            std::vector<float> d(size_t{w} * h);
            std::memcpy(d.data(), src, d.size() * 4);
            float lo = d[0], hi = d[0];
            double sum = 0;
            for (const float v : d) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
            }
            std::fprintf(stderr, ", depth min %.6f avg %.6f max %.6f centre %.6f\n", lo, sum / d.size(), hi,
                         d[size_t{h / 2} * w + w / 2]);
            const float range = hi > lo ? hi - lo : 1.f;
            for (size_t i = 0; i < d.size(); ++i)
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = static_cast<uint8_t>((d[i] - lo) / range * 255.f);
            char name[128];
            std::snprintf(name, sizeof(name), "%s_%ux%u_%llx_depth.bmp", base, w, h,
                          static_cast<unsigned long long>(image.info.guest_address));
            WriteBmp(name, rgb, w, h);
            std::fflush(stderr);
            return;
        }
        double sum = 0;
        for (size_t i = 0; i < size_t{w} * h; ++i) {
            DecodePixel(fmt, src + i * bpp, &rgb[i * 3]);
            sum += rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2];
        }
        std::fprintf(stderr, ", average %.1f/255\n", sum / (3.0 * w * h));
        char name[128];
        std::snprintf(name, sizeof(name), "%s_%ux%u_%llx.bmp", base, w, h,
                      static_cast<unsigned long long>(image.info.guest_address));
        WriteBmp(name, rgb, w, h);
        // 8-bit RGBA targets: also write alpha as a grey image (LBP3 keeps a glow mask there).
        if (fmt == vk::Format::eR8G8B8A8Unorm || fmt == vk::Format::eR8G8B8A8Srgb ||
            fmt == vk::Format::eB8G8R8A8Unorm || fmt == vk::Format::eB8G8R8A8Srgb) {
            double alphaSum = 0;
            for (size_t i = 0; i < size_t{w} * h; ++i) {
                const uint8_t a = src[i * 4 + 3];
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = a;
                alphaSum += a;
            }
            std::fprintf(stderr, "[targets]     alpha average %.1f/255\n", alphaSum / (double(w) * h));
            std::snprintf(name, sizeof(name), "%s_%ux%u_%llx_alpha.bmp", base, w, h,
                          static_cast<unsigned long long>(image.info.guest_address));
            WriteBmp(name, rgb, w, h);
        }
    }
    std::fflush(stderr);
}

// Waits for the GPU to finish what was submitted, or to block on a WAIT_REG_MEM: the
// game may satisfy that only after its submit call returns, as on the console, where
// submits are asynchronous (waiting there deadlocked the Newton cutscene and thrusters).
// LBP3_SYNC_STRICT=1 always waits for idle (for comparison).
void WaitGpu() {
    static const bool strict = std::getenv("LBP3_SYNC_STRICT") != nullptr;
    if (strict) g_liverpool->WaitGpuIdle();
    else g_liverpool->WaitGpuIdleOrBlocked();
}

uint32_t TakeEarlyReleases() { return g_liverpool ? g_liverpool->early_releases.exchange(0) : 0; }
uint32_t LearnedWaits() { return g_liverpool ? g_liverpool->learned_waits.load() : 0; }

void WaitIdle() {
    if (g_liverpool) WaitGpu();
}

// Ported from shadPS4's gnmdriver.cpp.
int MapComputeQueue(uint32_t pipeId, uint32_t queueId, uint64_t ringBase, uint32_t ringSizeDw, uint32_t* readPtr) {
    if (!g_liverpool || pipeId >= AmdGpu::Liverpool::NumComputePipes) return ORBIS_GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID;
    if (queueId >= AmdGpu::Liverpool::NumQueuesPerPipe) return ORBIS_GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID;
    if (ringBase % 256 != 0) return ORBIS_GNM_ERROR_COMPUTEQUEUE_INVALID_RING_BASE_ADDR;
    if (!std::has_single_bit(ringSizeDw)) return ORBIS_GNM_ERROR_COMPUTEQUEUE_INVALID_RING_SIZE;
    if (reinterpret_cast<uintptr_t>(readPtr) % 4 != 0) return ORBIS_GNM_ERROR_COMPUTEQUEUE_INVALID_READ_PTR_ADDR;
    const auto vqid = g_liverpool->asc_queues.Insert(VAddr(ringBase), readPtr, ringSizeDw, pipeId);
    *g_liverpool->asc_queues[vqid].read_addr = 0u;
    std::fprintf(stderr, "[gnm] compute pipe %u queue %u mapped to vqueue %u (ring %u dwords)\n", pipeId, queueId,
                 vqid.index + 1, ringSizeDw);
    return static_cast<int>(vqid.index + 1);  // DingDong takes ids in [1..64]
}

void UnmapComputeQueue(uint32_t vqid) {
    if (g_liverpool && vqid) g_liverpool->asc_queues.Erase(Common::SlotId{vqid - 1});
}

void DingDong(uint32_t gnmVqid, uint32_t nextOffsDw) {
    if (!g_liverpool || gnmVqid == 0) return;
    static std::mutex submitMutex;
    static uint32_t nextOffs[64]{};  // where each ring's unsubmitted commands start
    std::scoped_lock lk{submitMutex};
    WaitGpu();
    const uint32_t vqid = gnmVqid - 1;
    auto& queue = g_liverpool->asc_queues[{vqid}];
    auto& offs = nextOffs[vqid];
    if (nextOffsDw == offs) return;
    const auto* ring = reinterpret_cast<const uint32_t*>(queue.map_addr);
    if (nextOffsDw < offs && nextOffsDw != 0) {
        // The submission wraps at the end of the ring: submit it in two parts.
        g_liverpool->SubmitAsc(gnmVqid, {ring + offs, queue.ring_size_dw - offs});
        offs = 0;
    }
    const uint32_t sizeDw = (nextOffsDw ? nextOffsDw : queue.ring_size_dw) - offs;
    const std::span<const uint32_t> acb{ring + offs, sizeDw};
    offs = nextOffsDw;
    g_liverpool->SubmitAsc(gnmVqid, acb);
}

void SubmitGfx(const uint32_t* dcb, uint32_t dcbBytes, const uint32_t* ccb, uint32_t ccbBytes) {
    if (!g_liverpool) return;
    if (!g_sentInit) {
        g_sentInit = true;
        // LBP3 is SDK 2.00 on a base PS4; same choice as shadPS4's PerformSubmit.
        const uint32_t sdk = SdkVersion();
        if (sdk < Common::ElfInfo::FW_200) g_liverpool->SubmitGfx(Libraries::GnmDriver::InitSequence, {});
        else if (sdk < Common::ElfInfo::FW_400) g_liverpool->SubmitGfx(Libraries::GnmDriver::InitSequence200, {});
        else g_liverpool->SubmitGfx(Libraries::GnmDriver::InitSequence350, {});
    }
    // Parse a snapshot taken at submit time, as the console's command processor has in
    // effect read the commands before the game reuses that memory. LBP3 rewrites command
    // memory the GPU hadn't reached yet (a complete ACQUIRE_MEM found inside another one,
    // at the Bunkum checkpoint). Each submission keeps its own copy; 64 are far more than
    // can be in flight. LBP3_COPY_CMD_BUFFERS=off parses the game's memory directly.
    static const bool copy = [] {
        const char* v = std::getenv("LBP3_COPY_CMD_BUFFERS");
        return !v || std::string(v) != "off";
    }();
    std::span<const uint32_t> dcbSpan{dcb, dcbBytes / 4}, ccbSpan{ccb, ccb ? ccbBytes / 4 : 0};
    if (copy) {
        static std::array<std::vector<uint32_t>, 64> dcbCopies, ccbCopies;
        static size_t next = 0;
        auto& d = dcbCopies[next];
        auto& c = ccbCopies[next];
        next = (next + 1) % dcbCopies.size();
        d.assign(dcbSpan.begin(), dcbSpan.end());
        c.assign(ccbSpan.begin(), ccbSpan.end());
        dcbSpan = d;
        ccbSpan = c;
    }
    g_liverpool->SubmitGfx(dcbSpan, ccbSpan);
    // Finish each submission before returning to the game. With the command
    // processor running behind the game, LBP3's Bink video decoder rewrote a
    // texture while our GPU was still copying it, tearing the intro video.
    // LBP3_GPU_ASYNC=1 restores the overlap (faster; for testing).
    static const bool async = std::getenv("LBP3_GPU_ASYNC") != nullptr;
    if (!async) WaitGpu();
}

}  // namespace gpu
