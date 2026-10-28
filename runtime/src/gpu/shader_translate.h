// Runs shadPS4's shader recompiler on shaders the game draws with, at draw
// time (descriptors referenced by user data are only valid then).
#pragma once
#include <cstdint>

namespace gpu {

// shRegs/ctxRegs: the PM4 register shadows (SH and context register files).
// Translates the bound pixel shader to SPIR-V the first time it's seen and
// writes analysis/shaders/ps_<hash>.spv. Returns false if translation failed.
bool TranslateBoundPixelShader(const uint32_t* shRegs, const uint32_t* ctxRegs);

}  // namespace gpu
