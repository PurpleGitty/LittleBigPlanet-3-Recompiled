# shadPS4's GCN -> SPIR-V shader recompiler, built unmodified from the
# shadPS4 tree (GPL-2.0-or-later). Our stand-ins for the few emulator hooks it
# touches live in runtime/shim (headers, searched first) and
# runtime/src/gpu/shadps4_glue.cpp.

set(SHADPS4 ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/shadPS4)
set(SHADPS4_EXT ${SHADPS4}/externals)

# --- dependencies (all pinned by shadPS4's submodules) -------------------------
# Some of these add MSVC-only flags (/MP) with warnings-as-errors; clang-cl
# would reject them as unused.
string(APPEND CMAKE_CXX_FLAGS " -Wno-unused-command-line-argument")
string(APPEND CMAKE_C_FLAGS " -Wno-unused-command-line-argument")
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_EXT}/fmt ${CMAKE_BINARY_DIR}/ext/fmt EXCLUDE_FROM_ALL)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_EXT}/sirit ${CMAKE_BINARY_DIR}/ext/sirit EXCLUDE_FROM_ALL)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_EXT}/zydis ${CMAKE_BINARY_DIR}/ext/zydis EXCLUDE_FROM_ALL)

# --- the recompiler -------------------------------------------------------------
file(GLOB_RECURSE RECOMPILER_SOURCES CONFIGURE_DEPENDS ${SHADPS4}/src/shader_recompiler/*.cpp)
add_library(shader_recompiler STATIC
    ${RECOMPILER_SOURCES}
    ${SHADPS4}/src/common/decoder.cpp
    ${SHADPS4}/src/common/io_file.cpp
    ${SHADPS4}/src/common/string_util.cpp
    ${SHADPS4}/src/common/ntapi.cpp
    ${SHADPS4}/src/common/signal_context.cpp
    ${SHADPS4}/src/video_core/amdgpu/pixel_format.cpp
    src/gpu/shadps4_glue.cpp
    src/gpu/shader_translate.cpp
)
set_target_properties(shader_recompiler PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(shader_recompiler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/shim          # our stand-ins win over shadPS4's headers
        ${SHADPS4}/src
        ${SHADPS4}                                # a few headers say "src/common/..."
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src           # our gpu/ glue
    PUBLIC
        ${SHADPS4}/src/shader_recompiler/frontend  # for the "opcodes.inc"-style relative includes
        ${SHADPS4_EXT}/ext-boost
        ${SHADPS4_EXT}/robin-map/include
        ${SHADPS4_EXT}/magic_enum/include
        ${SHADPS4_EXT}/half/include
        ${SHADPS4_EXT}/xxhash
        ${SHADPS4_EXT}/xbyak
        ${SHADPS4_EXT}/vulkan-headers/include
)
target_compile_definitions(shader_recompiler PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
    _SCL_SECURE_NO_WARNINGS _TIMESPEC_DEFINED BOOST_ASIO_STANDALONE XXH_INLINE_ALL
    NTDDI_VERSION=0x0A000006 _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(shader_recompiler PRIVATE -Wno-everything)
target_link_libraries(shader_recompiler PUBLIC fmt::fmt sirit Zydis)
