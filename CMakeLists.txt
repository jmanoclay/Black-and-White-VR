cmake_minimum_required(VERSION 3.24)
project(bwvr LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Black & White is a 32-bit game: configure with an x86 toolchain (vcvarsall x86).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL has no runtime dependencies in the game folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS DIRECT3D_VERSION=0x0700)

# --- MinHook (built from source) ---
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- ddraw.dll proxy: the mod itself ---
add_library(bwvr_ddraw SHARED
    src/proxy/exports.cpp
    src/proxy/ddraw.def
    src/proxy/version.rc
    src/core/log.cpp
    src/core/paths.cpp
    src/core/exe_check.cpp
    src/core/mod.cpp
    src/core/audio_balance.cpp
    src/core/audio_stop.cpp
    src/core/load_trace.cpp
    src/control/input_inject.cpp
    src/control/vr_hand.cpp
    src/game/camdump.cpp
    src/render/d3d7_trace.cpp
    src/render/screenshot.cpp
    src/render/eye_target.cpp
    src/render/d3d9_bridge.cpp
    src/render/mipmaps.cpp
    src/render/stereo.cpp
    src/ui/debug_menu.cpp
    src/ui/save_name.cpp
    src/render/video_screen.cpp
    src/ui/ui.cpp
    src/vr/hmd_profile.cpp
    src/vr/openvr_backend.cpp)
set_target_properties(bwvr_ddraw PROPERTIES OUTPUT_NAME ddraw)
target_include_directories(bwvr_ddraw PRIVATE src third_party/openvr/headers)
target_link_libraries(bwvr_ddraw PRIVATE minhook dxguid d3d11 dxgi delayimp winmm
    ${CMAKE_SOURCE_DIR}/third_party/openvr/lib/win32/openvr_api.lib)
target_compile_options(bwvr_ddraw PRIVATE /W4 /wd4100 /Zi)
# Development build (on): the pause menu's Developer tab (give a miracle, every tuning setting) and the
# left hand's tuning panel. Player releases are built with it off (toolsuild.ps1 -Player).
option(BWVR_DEVELOPER "Developer features: the Developer tab and the tuning panel" ON)
target_compile_definitions(bwvr_ddraw PRIVATE BWVR_DEVELOPER=$<BOOL:${BWVR_DEVELOPER}>)
# openvr_api.dll is delay-loaded: the mod still works (mono, mock VR) without it.
target_link_options(bwvr_ddraw PRIVATE /DEBUG /SAFESEH:NO /DELAYLOAD:openvr_api.dll)

# --- Headset probe: records the HMD's optics to profiles/*.json ---
add_executable(bwvr_probe tools/probe/probe.cpp)
target_include_directories(bwvr_probe PRIVATE third_party/openvr/headers)
target_link_libraries(bwvr_probe PRIVATE ${CMAKE_SOURCE_DIR}/third_party/openvr/lib/win32/openvr_api.lib)
add_custom_command(TARGET bwvr_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/openvr/bin/win32/openvr_api.dll $<TARGET_FILE_DIR:bwvr_probe>)
