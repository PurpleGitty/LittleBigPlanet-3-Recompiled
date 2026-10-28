// Keyboard (and later mouse) controls as named game actions, not as an emulated pad.
//
// Each action ("jump", "open_popit", ...) is bound to keys in user/config/input.ini and
// has its own behaviour below. Until the game's own functions for an action are found
// (character movement, opening the Popit or the Pod computer), that behaviour feeds the
// game's per-frame control input, the only way into the game we have so far; actions are
// moved over to direct game calls one at a time without changing bindings.
#pragma once
#include <cstdint>
#include <string>

#include "hle/pad_types.h"

namespace input::actions {

enum class Action {
    // Character
    MoveLeft,
    MoveRight,
    LayerBack,   // move one layer away from the camera
    LayerFront,  // move one layer towards the camera
    Jump,
    Grab,
    // Pod and creation
    OpenComputer,
    OpenPopit,
    // Menus
    MenuUp,
    MenuDown,
    MenuLeft,
    MenuRight,
    Confirm,
    Back,
    Pause,
    Count,
};

// Loads bindings (writing a commented default file the first time).
void Init();
// True while the game window has keyboard focus and some binding is held.
bool AnyHeld();
// Holds an action for `ms` as if its key were down (remote-controlled test runs).
// Returns false for an unknown action name.
bool HoldForTest(const std::string& name, uint64_t ms);
// Applies the held actions to this frame's control input for player 1.
void ApplyToFrame(ps4::PadData& frame);

}  // namespace input::actions
