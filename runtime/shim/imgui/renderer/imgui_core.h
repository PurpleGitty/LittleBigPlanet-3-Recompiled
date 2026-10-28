// Stand-in: shadPS4's ImGui overlay isn't used here.
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"

namespace ImGui::Core {
inline void Shutdown(const vk::Device&) {}
}  // namespace ImGui::Core
