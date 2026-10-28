// Minimal PM4 (AMD GPU command packet) processing.
//
// Draws are ignored for now; only packets that write to memory or raise
// interrupts are executed, because the game's CPU code waits on those
// (fence labels, end-of-pipe callbacks). Packet layouts follow shadPS4's
// video_core/amdgpu/pm4_cmds.h (GPL-2.0-or-later).
#pragma once
#include <cstddef>
#include <cstdint>

namespace hle::pm4 {

struct Result {
    bool interrupt = false;  // an end-of-pipe/release packet asked for an IRQ
    size_t labelWrites = 0;
    size_t draws = 0, dispatches = 0;
    uint64_t lastColorTarget = 0;  // CB_COLOR0_BASE (bytes) at the last draw
    uint32_t lastTargetPitch = 0;  // CB_COLOR0_PITCH raw
};

// Offset (dwords) of the first packet that runs past the end, or -1 if the
// buffer's packets tile it exactly.
long FindOverrun(const uint32_t* cmds, size_t dwords);

// Walks a command buffer (and any indirect buffers it calls).
Result Execute(const uint32_t* cmds, size_t sizeBytes);

}  // namespace hle::pm4
