# shadPS4's video_core (PM4 command processor, Vulkan renderer, texture and
# buffer caches), built from the shadPS4 tree (GPL-2.0-or-later). Presentation
# (vk_presenter/vk_swapchain, which pull in shadPS4's ImGui UI) is replaced by
# our own; other emulator hooks are stand-ins in runtime/shim and
# runtime/src/gpu/video_core_glue.cpp.
# Requires shader_recompiler.cmake (SHADPS4, SHADPS4_EXT, fmt, sirit, Zydis).

# --- dependencies ---------------------------------------------------------------
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_EXT}/vulkan-headers ${CMAKE_BINARY_DIR}/ext/vulkan-headers EXCLUDE_FROM_ALL)
add_subdirectory(${SHADPS4_EXT}/vma ${CMAKE_BINARY_DIR}/ext/vma EXCLUDE_FROM_ALL)

set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_EXT}/glslang ${CMAKE_BINARY_DIR}/ext/glslang EXCLUDE_FROM_ALL)

add_subdirectory(${SHADPS4_EXT}/miniz ${CMAKE_BINARY_DIR}/ext/miniz EXCLUDE_FROM_ALL)

# Build-time GLSL helpers (detiler, blits, post-processing) -> embedded SPIR-V headers.
add_subdirectory(${SHADPS4}/src/video_core/host_shaders ${CMAKE_BINARY_DIR}/host_shaders)

# --- video_core -----------------------------------------------------------------
file(GLOB_RECURSE VIDEO_CORE_SOURCES CONFIGURE_DEPENDS ${SHADPS4}/src/video_core/*.cpp)
# Presentation (and its post-processing/FSR passes) is replaced by our own.
list(FILTER VIDEO_CORE_SOURCES EXCLUDE REGEX "vk_presenter\\.cpp$|vk_swapchain\\.cpp$|renderdoc\\.cpp$|host_passes/")

add_library(video_core STATIC
    ${VIDEO_CORE_SOURCES}
    ${SHADPS4}/src/common/error.cpp
    ${SHADPS4}/src/common/slot_vector.cpp
    ${SHADPS4}/src/common/number_utils.cpp
    ${SHADPS4}/src/common/recursive_lock.cpp
    ${SHADPS4}/src/common/spin_lock.cpp
    src/gpu/video_core_glue.cpp
    src/gpu/renderer.cpp
)
add_dependencies(video_core host_shaders)
set_target_properties(video_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(video_core
    PRIVATE ${HOST_SHADERS_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC ${SHADPS4_EXT}/tracy/public)  # profiling compiled out (no TRACY_ENABLE)
target_compile_options(video_core PRIVATE -Wno-everything)
target_link_libraries(video_core PUBLIC shader_recompiler Vulkan::Headers GPUOpen::VulkanMemoryAllocator SDL3::SDL3 miniz)
