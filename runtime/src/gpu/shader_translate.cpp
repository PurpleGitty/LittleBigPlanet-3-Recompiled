// Fragment runtime-info setup follows shadPS4's PipelineCache::BuildRuntimeInfo
// (GPL-2.0-or-later), fed from our register shadows instead of its Liverpool model.
#include "gpu/shader_translate.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_shader.h"

namespace gpu {
namespace {

// Register indices inside the SH / context files.
constexpr uint32_t kShPsProgram = 0x8;        // SPI_SHADER_PGM_LO_PS (start of AmdGpu::ShaderProgram)
constexpr uint32_t kCtxPsInputCntl0 = 0x191;  // SPI_PS_INPUT_CNTL_0..31
constexpr uint32_t kCtxPsInputEna = 0x1B3;
constexpr uint32_t kCtxPsInputAddr = 0x1B4;
constexpr uint32_t kCtxPsInControl = 0x1B6;   // num_interp in bits 0..5
constexpr uint32_t kCtxZExportFormat = 0x1C4;

std::mutex g_mutex;
std::set<uint64_t> g_done;
Shader::Pools g_pools;

Shader::Profile MakeProfile() {
    // A conservative desktop Vulkan 1.3 profile; refined once a real device is queried.
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.subgroup_size = 32;
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 48 * 1024;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_float16 = p.support_float64 = true;
    p.supports_image_load_store_lod = false;
    return p;
}

}  // namespace

bool TranslateBoundPixelShader(const uint32_t* sh, const uint32_t* ctx) {
    const auto& pgm = *reinterpret_cast<const AmdGpu::ShaderProgram*>(sh + kShPsProgram);
    if (!pgm.address) return false;
    const Shader::ShaderParams params = AmdGpu::GetParams(pgm);

    std::lock_guard lock(g_mutex);
    if (!g_done.insert(params.hash).second) return true;

    Shader::RuntimeInfo rt{};
    rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    rt.props.fp_denorm_mode32 = pgm.settings.fp_denorm_mode32;
    rt.props.fp_denorm_mode16_64 = pgm.settings.fp_denorm_mode64;
    rt.props.fp_round_mode32 = pgm.settings.fp_round_mode32;
    rt.props.fp_round_mode16_64 = pgm.settings.fp_round_mode64;
    rt.props.num_allocated_vgprs = pgm.NumVgprs();
    rt.props.num_user_data = pgm.settings.num_user_regs;
    rt.props.num_input_vgprs = pgm.settings.vgpr_comp_cnt;
    rt.props.dx10_clamp = pgm.settings.dx10_clamp;

    auto& fs = rt.hw.fs;
    fs.en_flags = std::bit_cast<AmdGpu::PsInput>(ctx[kCtxPsInputEna]);
    fs.addr_flags = std::bit_cast<AmdGpu::PsInput>(ctx[kCtxPsInputAddr]);
    fs.num_inputs = ctx[kCtxPsInControl] & 0x3F;
    fs.num_samples = 1;
    fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(ctx[kCtxZExportFormat] & 0xF);
    for (uint32_t i = 0; i < fs.num_inputs; ++i) {
        const auto in = std::bit_cast<AmdGpu::PsInputControl>(ctx[kCtxPsInputCntl0 + i]);
        fs.inputs[i] = {.param_index = uint16_t(in.input_offset),
                        .is_default = uint16_t(in.use_default),
                        .is_flat = uint16_t(in.flat_shade),
                        .default_value = uint16_t(in.default_value)};
    }

    static const Shader::Profile profile = MakeProfile();
    try {
        Shader::Info info(Shader::HwStage::Fragment, Shader::SwStage::Fragment, params);
        const auto program = Shader::TranslateProgram(params.code, g_pools, info, rt, profile);
        Shader::Backend::Bindings bindings{};
        const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
        g_pools.ReleaseContents();

        std::filesystem::create_directories("analysis/shaders");
        char name[96];
        std::snprintf(name, sizeof(name), "analysis/shaders/ps_%016llx.spv",
                      static_cast<unsigned long long>(params.hash));
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(spv.data(), sizeof(uint32_t), spv.size(), f);
            std::fclose(f);
        }
        std::fprintf(stderr, "[gpu] pixel shader %016llx: %zu dwords GCN -> %zu dwords SPIR-V\n",
                     static_cast<unsigned long long>(params.hash), params.code.size(), spv.size());
        return true;
    } catch (const std::exception& e) {
        g_pools.ReleaseContents();
        std::fprintf(stderr, "[gpu] pixel shader %016llx failed: %s\n", static_cast<unsigned long long>(params.hash),
                     e.what());
        return false;
    }
}

}  // namespace gpu
