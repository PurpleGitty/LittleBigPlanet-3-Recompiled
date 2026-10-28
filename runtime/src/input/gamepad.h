// Host gamepad backend. Translates whatever controller the PC has into
// PS4 pad state, so the HLE libScePad layer never touches Windows APIs.
#pragma once
#include "hle/pad_types.h"

namespace input {

constexpr int kMaxPads = 4;

void Init();
// Fills `out` for the given player slot. Returns false if nothing is connected there.
bool Read(int slot, ps4::PadData& out);
void SetVibration(int slot, uint8_t largeMotor, uint8_t smallMotor);
void SetLightBar(int slot, uint8_t r, uint8_t g, uint8_t b);

}  // namespace input
