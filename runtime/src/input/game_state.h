// What the game is showing, asked from the game itself: the functions behind its own
// script natives (AnyPoppetActive__, IsStartMenuOpen__, GetPodComputerInUse__). Only call
// these from a game thread (the game reads input on one, so actions are fine).
#pragma once

namespace input::game_state {

bool PopitOpen();         // any Popit is open
bool StartMenuOpen();     // the pause / start menu is open
bool PodComputerInUse();  // the Pod computer's menus are up
void UsePodComputer();    // opens the Pod computer, as the Pod script does on Square

// Hooks the Pod script's computer request (once, at start-up, before the game runs) so
// BlockComputerFromSquare(true) stops a Square press from opening the computer.
void InstallHooks();

// Call once a frame from a game thread: ends a cutscene level that stays on after its
// last cutscene camera (The Legend of Bunkum), as the pause menu's skip does.
void CutSceneWatchdog();
void BlockComputerFromSquare(bool block);

}  // namespace input::game_state
