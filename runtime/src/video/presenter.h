// The game window. Shows whichever display buffer the game flipped last.
//
// Display buffers are what the GPU renders into. Until draw commands are
// executed they hold nothing meaningful, and they're usually tiled, so the
// picture is garbage for now; the window, buffer registration and pacing are
// what's real here.
#pragma once
#include <cstdint>
#include <vector>

namespace video {

struct DisplayBuffer {
    const void* address = nullptr;
    uint32_t width = 0, height = 0, pitchPixels = 0;
    uint32_t pixelFormat = 0, tilingMode = 0;
};

// Opens the window on its own thread. Closing it exits the game.
void StartPresenter();
// Debug hotkeys (F9, F10...) pressed in the game window, as SDL keycodes.
void SetHotkeyHandler(void (*handler)(int key));
// Runs the hotkey handler as if `key` was pressed (remote-controlled test runs).
void TriggerHotkey(int key);
void SetDisplayBuffer(int index, const DisplayBuffer& buffer);
void PresentBuffer(int index);

// A finished frame from the GPU renderer (BGRA8, tightly packed). Once frames
// arrive this way, they're shown instead of the raw display buffers.
// Writes the next `count` presented frames to analysis\frames (quarter size), with
// their hand-over times in times.txt (also LBP3_DUMP_FRAMES=skip:count at startup).
void RequestFrameDump(int count);

// Hands over a frame; pixels comes back holding an older buffer to reuse.
void PresentPixels(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height);

// Blocks until the next flip slot on the window's real vsync for a game running
// at gameHz (e.g. every 2nd refresh for 30 fps on 60 Hz). Returns false when
// that doesn't divide evenly or the window isn't presenting; the caller then
// paces with a timer.
bool WaitForFlipSlot(double gameHz);

}  // namespace video
