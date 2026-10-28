// Stand-in for shadPS4's sdl_window.h. The renderer runs headless; our own
// window (video/presenter.cpp) shows the frames. Types copied from shadPS4.
#pragma once
#include "common/types.h"

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSystemInfo GetWindowInfo() const { return info; }
    WindowSystemInfo info{};
};

}  // namespace Frontend
