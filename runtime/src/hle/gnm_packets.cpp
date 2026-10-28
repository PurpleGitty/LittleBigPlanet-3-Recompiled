// libSceGnmDriver functions that write GPU commands into space the game has
// reserved in a command buffer (shader setup, hardware init, markers...).
//
// They must fill exactly the reserved space or the command stream falls out
// of sync. Until rendering exists, each one fills its space with a single NOP
// packet; the real register writes come with the GPU work (shadPS4's
// gnmdriver.cpp has them). Sizes follow shadPS4.
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "core/libraries/gnmdriver/gnmdriver_init.h"

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::gnm {
namespace {

constexpr uint32_t kHwInitPacketSize = 0x100;  // dwords
constexpr uint32_t kWaitFlipDoneSize = 7;
constexpr uint32_t kOpNop = 0x10;

constexpr uint32_t kOpSetContextReg = 0x69;
constexpr uint32_t kOpSetShReg = 0x76;
constexpr uint32_t kOpAcquireMem = 0x58;

uint32_t Type3(uint32_t op, uint32_t bodyDwords) { return (3u << 30) | ((bodyDwords - 1) << 16) | (op << 8); }

// Writes SET_*_REG starting at `reg` with the given values; returns the next dword.
uint32_t* SetRegs(uint32_t* cmd, uint32_t op, uint32_t reg, std::initializer_list<uint32_t> values) {
    *cmd++ = Type3(op, 1 + static_cast<uint32_t>(values.size()));
    *cmd++ = reg;
    for (uint32_t v : values) *cmd++ = v;
    return cmd;
}

uint32_t* ShReg(uint32_t* cmd, uint32_t reg, std::initializer_list<uint32_t> v) { return SetRegs(cmd, kOpSetShReg, reg, v); }
// SET_SH_REG flagged for the compute pipe (header bit 1).
uint32_t* CsShReg(uint32_t* cmd, uint32_t reg, std::initializer_list<uint32_t> v) {
    uint32_t* out = SetRegs(cmd, kOpSetShReg, reg, v);
    cmd[0] |= 1u << 1;
    return out;
}
uint32_t* CtxReg(uint32_t* cmd, uint32_t reg, std::initializer_list<uint32_t> v) {
    return SetRegs(cmd, kOpSetContextReg, reg, v);
}

void FillNop(uint32_t* cmd, uint32_t dwords) {
    if (!cmd || dwords == 0) return;
    if (dwords == 1) {
        cmd[0] = 0x80000000u;  // type-2 filler
        return;
    }
    // Type-3 NOP: count field = body dwords - 1 = dwords - 2.
    cmd[0] = (3u << 30) | ((dwords - 2) << 16) | (kOpNop << 8);
}

}  // namespace

int PS4_SYSV_ABI sceGnmInsertWaitFlipDone(uint32_t* cmd, uint32_t size, int32_t, uint32_t) {
    if (size != kWaitFlipDoneSize) return -1;
    FillNop(cmd, size);
    return kernel::SCE_OK;
}

// All of these take (cmd, size, ...) and own exactly `size` dwords.
#define FILL_FUNCTION(name, ...)                                     \
    int PS4_SYSV_ABI name(uint32_t* cmd, uint32_t size, ##__VA_ARGS__) { \
        if (!cmd) return -1;                                         \
        FillNop(cmd, size);                                          \
        return kernel::SCE_OK;                                       \
    }

FILL_FUNCTION(sceGnmInsertPopMarker)
FILL_FUNCTION(sceGnmInsertPushMarker, const char*)
FILL_FUNCTION(sceGnmInsertSetMarker, const char*)
FILL_FUNCTION(sceGnmInsertPushColorMarker, const char*, uint32_t)
FILL_FUNCTION(sceGnmInsertSetColorMarker, const char*, uint32_t)
FILL_FUNCTION(sceGnmUpdatePsShader, const uint32_t*)
FILL_FUNCTION(sceGnmUpdateVsShader, const uint32_t*, uint32_t)
FILL_FUNCTION(sceGnmSetEsShader, const uint32_t*, uint32_t)
FILL_FUNCTION(sceGnmSetGsShader, const uint32_t*)
FILL_FUNCTION(sceGnmUpdateGsShader, const uint32_t*)
FILL_FUNCTION(sceGnmSetHsShader, const uint32_t*, uint32_t)
FILL_FUNCTION(sceGnmUpdateHsShader, const uint32_t*, uint32_t)
FILL_FUNCTION(sceGnmSetLsShader, const uint32_t*, uint32_t)

#undef FILL_FUNCTION

// Hardware init, ported from shadPS4. Each fills exactly kHwInitPacketSize dwords.
uint32_t PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState200(uint32_t* cmd, uint32_t size) {
    if (size < kHwInitPacketSize) return 0;
    uint32_t* const end = cmd + kHwInitPacketSize;
    // CLEAR_STATE plus the context defaults shadPS4 uses for this SDK (base PS4).
    static constexpr uint32_t kClearState[] = {0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
                                               0x2ec47fc0u, 0xffffffffu, 0u,          0u,          0u, 10u};
    std::memcpy(cmd, kClearState, sizeof(kClearState));
    cmd += std::size(kClearState);
    const auto& seq = Libraries::GnmDriver::InitSequence200;
    std::memcpy(cmd, seq.data() + 2, (seq.size() - 2) * 4);
    cmd += seq.size() - 2;
    FillNop(cmd, static_cast<uint32_t>(end - cmd));
    return kHwInitPacketSize;
}

uint32_t PS4_SYSV_ABI sceGnmDispatchInitDefaultHardwareState(uint32_t* cmd, uint32_t size) {
    if (size < kHwInitPacketSize) return 0;
    uint32_t* const end = cmd + kHwInitPacketSize;
    cmd = CsShReg(cmd, 0x216, {0xFFFFFFFFu});  // COMPUTE_STATIC_THREAD_MGMT_SE0
    cmd = CsShReg(cmd, 0x217, {0xFFFFFFFFu});  // COMPUTE_STATIC_THREAD_MGMT_SE1
    cmd = CsShReg(cmd, 0x215, {0x170u});       // COMPUTE_RESOURCE_LIMITS
    *cmd++ = Type3(kOpAcquireMem, 6);
    for (uint32_t v : {0x28000000u, 0u, 0u, 0u, 0u, 0xAu}) *cmd++ = v;
    FillNop(cmd, static_cast<uint32_t>(end - cmd));
    return kHwInitPacketSize;
}

int PS4_SYSV_ABI sceGnmSetCsShader(uint32_t* cmd, uint32_t size, const uint32_t* cs) {
    if (!cmd || size <= 0x18 || !cs || cs[1] != 0) return -1;
    uint32_t* const end = cmd + size;
    cmd = ShReg(cmd, 0x20C, {cs[0], 0});             // COMPUTE_PGM_LO/HI
    cmd = ShReg(cmd, 0x212, {cs[2], cs[3]});         // COMPUTE_PGM_RSRC1/2
    cmd = ShReg(cmd, 0x207, {cs[4], cs[5], cs[6]});  // COMPUTE_NUM_THREAD_X/Y/Z
    FillNop(cmd, static_cast<uint32_t>(end - cmd));
    return kernel::SCE_OK;
}

// Shader setup with the real register writes (ported from shadPS4's gnmdriver.cpp).
// The rest of each reserved block is a trailing NOP, as on the console.
int PS4_SYSV_ABI sceGnmSetPsShader(uint32_t* cmd, uint32_t size, const uint32_t* ps) {
    if (!cmd || size <= 0x27) return -1;
    uint32_t* const end = cmd + size;
    if (!ps) {
        cmd = ShReg(cmd, 0x8, {0, 0});   // SPI_SHADER_PGM_LO/HI_PS
        cmd = CtxReg(cmd, 0x203, {0});   // DB_SHADER_CONTROL
    } else {
        if (ps[1] != 0) return -1;
        cmd = ShReg(cmd, 0x8, {ps[0], 0});          // SPI_SHADER_PGM_LO/HI_PS
        cmd = ShReg(cmd, 0xA, {ps[2], ps[3]});      // SPI_SHADER_PGM_RSRC1/2_PS
        cmd = CtxReg(cmd, 0x1C4, {ps[4], ps[5]});   // SPI_SHADER_Z_FORMAT / COL_FORMAT
        cmd = CtxReg(cmd, 0x1B3, {ps[6], ps[7]});   // SPI_PS_INPUT_ENA / ADDR
        cmd = CtxReg(cmd, 0x1B6, {ps[8]});          // SPI_PS_IN_CONTROL
        cmd = CtxReg(cmd, 0x1B8, {ps[9]});          // SPI_BARYC_CNTL
        cmd = CtxReg(cmd, 0x203, {ps[10]});         // DB_SHADER_CONTROL
        cmd = CtxReg(cmd, 0x8F, {ps[11]});          // CB_SHADER_MASK
    }
    FillNop(cmd, static_cast<uint32_t>(end - cmd));
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceGnmSetVsShader(uint32_t* cmd, uint32_t size, const uint32_t* vs, uint32_t modifier) {
    if (!cmd || size <= 0x1C || !vs || (modifier & 0xFCFFFC3F) || vs[1] != 0) return -1;
    uint32_t* const end = cmd + size;
    const uint32_t rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xFCFFFC3F) | modifier;
    cmd = ShReg(cmd, 0x48, {vs[0], 0});       // SPI_SHADER_PGM_LO/HI_VS
    cmd = ShReg(cmd, 0x4A, {rsrc1, vs[3]});   // SPI_SHADER_PGM_RSRC1/2_VS
    cmd = CtxReg(cmd, 0x207, {vs[6]});        // PA_CL_VS_OUT_CNTL
    cmd = CtxReg(cmd, 0x1B1, {vs[4]});        // SPI_VS_OUT_CONFIG
    cmd = CtxReg(cmd, 0x1C3, {vs[5]});        // SPI_SHADER_POS_FORMAT
    FillNop(cmd, static_cast<uint32_t>(end - cmd));
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceGnmIsUserPaEnabled() { return 0; }

}  // namespace hle::gnm

namespace hle {

void RegisterGnmPackets(core::Linker& linker) {
    using namespace gnm;
    const char* lib = "libSceGnmDriver";
    HLE_REG(linker, lib, sceGnmDrawInitDefaultHardwareState200);
    HLE_REG(linker, lib, sceGnmDispatchInitDefaultHardwareState);
    HLE_REG(linker, lib, sceGnmInsertWaitFlipDone);
    HLE_REG(linker, lib, sceGnmInsertPopMarker);
    HLE_REG(linker, lib, sceGnmInsertPushMarker);
    HLE_REG(linker, lib, sceGnmInsertSetMarker);
    HLE_REG(linker, lib, sceGnmInsertPushColorMarker);
    HLE_REG(linker, lib, sceGnmInsertSetColorMarker);
    HLE_REG(linker, lib, sceGnmSetCsShader);
    HLE_REG(linker, lib, sceGnmSetPsShader);
    HLE_REG(linker, lib, sceGnmUpdatePsShader);
    HLE_REG(linker, lib, sceGnmSetVsShader);
    HLE_REG(linker, lib, sceGnmUpdateVsShader);
    HLE_REG(linker, lib, sceGnmSetEsShader);
    HLE_REG(linker, lib, sceGnmSetGsShader);
    HLE_REG(linker, lib, sceGnmUpdateGsShader);
    HLE_REG(linker, lib, sceGnmSetHsShader);
    HLE_REG(linker, lib, sceGnmUpdateHsShader);
    HLE_REG(linker, lib, sceGnmSetLsShader);
    HLE_REG(linker, lib, sceGnmIsUserPaEnabled);
}

}  // namespace hle
