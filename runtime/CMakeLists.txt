cmake_minimum_required(VERSION 3.25)
project(lbp3_runtime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Build with clang-cl: game-facing functions need the System V ABI")
endif()

find_package(SDL3 REQUIRED CONFIG PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/SDL3/cmake" NO_DEFAULT_PATH)

include(cmake/shader_recompiler.cmake)
include(cmake/video_core.cmake)

add_library(hle STATIC
    src/core/debugger.cpp
    src/core/linker.cpp
    src/core/module.cpp
    src/core/nid.cpp
    src/core/patches.cpp
    src/core/runner.cpp
    src/core/tls.cpp
    src/core/vfs.cpp
    src/hle/hle.cpp
    src/hle/app_content.cpp
    src/hle/audioout.cpp
    src/hle/trophy.cpp
    src/hle/fiber.cpp
    src/hle/dialogs.cpp
    src/hle/gnm_packets.cpp
    src/hle/http_offline.cpp
    src/hle/kernel_fs.cpp
    src/hle/kernel_memory.cpp
    src/hle/kernel_module.cpp
    src/hle/kernel_pthread.cpp
    src/hle/kernel_sync.cpp
    src/hle/kernel_time.cpp
    src/hle/libkernel.cpp
    src/hle/libScePad.cpp
    src/hle/net_offline.cpp
    src/hle/np_offline.cpp
    src/hle/playgo.cpp
    src/hle/pm4.cpp
    src/hle/rtc.cpp
    src/hle/savedata.cpp
    src/hle/system_services.cpp
    src/hle/videoout.cpp
    src/input/gamepad_sdl.cpp
    src/input/actions.cpp
    src/input/game_state.cpp
    src/core/native_trace.cpp
    src/video/presenter.cpp
)
target_include_directories(hle PUBLIC src)
target_link_libraries(hle PUBLIC shader_recompiler video_core)
target_compile_definitions(hle PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(hle PUBLIC SDL3::SDL3 bcrypt winmm dbghelp onecore advapi32)

add_executable(lbp3 src/main.cpp)
target_link_libraries(lbp3 PRIVATE hle)

add_executable(padtest tests/padtest.cpp)
target_link_libraries(padtest PRIVATE hle)

foreach(exe lbp3 padtest)
    add_custom_command(TARGET ${exe} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${exe}>)
endforeach()
