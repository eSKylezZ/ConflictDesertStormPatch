cmake_minimum_required(VERSION 3.21)
project(DesertStormFix VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "DesertStorm.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# Folder containing DesertStorm.exe; when set, every build copies the output there.
set(DS_GAME_DIR "" CACHE PATH "Conflict: Desert Storm install folder (optional deploy target)")
# Distribution build: no log file, no debug output, no PDB. Packages dist/ + a zip.
option(DS_DIST "Build the release package (no diagnostics)" OFF)

configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

add_library(dinput8 SHARED
    src/dllmain.cpp
    src/dinput8.def
    src/core/log.cpp
    src/core/patch.cpp
    src/core/settings.cpp
    src/core/gamepad.cpp
    src/features/widescreen.cpp
    src/features/hud.cpp
    src/features/controller.cpp
    src/features/timing.cpp
    src/features/framecap.cpp
    src/features/launcher.cpp
    src/features/launcher_pad.cpp
    src/features/ingame_input.cpp
    src/features/splitscreen.cpp
    src/features/dev.cpp
    src/features/discord.cpp
    src/features/display.cpp
    src/features/cinematics.cpp
    src/features/graphics.cpp
    src/features/coop.cpp
    src/features/overlay.cpp
    src/features/fontsharp.cpp
    src/features/padlayout.cpp
    src/features/controls.cpp
    src/features/padio.cpp
    src/features/mouse.cpp
    src/features/tooltips.cpp
    src/features/rumble.cpp
    src/features/audio.cpp
    ${CMAKE_BINARY_DIR}/version.rc
    src/game_icons.rc
)
target_include_directories(dinput8 PRIVATE src)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DS_VERSION="${PROJECT_VERSION}" $<$<BOOL:${DS_DIST}>:DS_DIST>)
target_compile_options(dinput8 PRIVATE /W4 /permissive- /utf-8)
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
target_link_libraries(dinput8 PRIVATE winmm msimg32 dxguid dinput8.lib ole32 windowscodecs)
target_link_options(dinput8 PRIVATE /SAFESEH:NO $<$<BOOL:${DS_DIST}>:/DEBUG:NONE>)

if(DS_DIST)
    set(DIST_DIR ${CMAKE_BINARY_DIR}/package)
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStormFix-${PROJECT_VERSION}.zip)
    # Relink (and so re-package) when the readme changes, not only when code does.
    set_property(TARGET dinput8 APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/dist/DesertStormFix-README.txt")
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${CMAKE_SOURCE_DIR}/dist/DesertStormFix-README.txt" "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${DIST_DIR}" ${CMAKE_COMMAND} -E tar cf "${DIST_ZIP}" --format=zip .
        COMMENT "Packaging ${DIST_ZIP}")
    if(DS_GAME_DIR)
        add_custom_command(TARGET dinput8 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIST_DIR}" "${DS_GAME_DIR}"
            COMMENT "Installing package into ${DS_GAME_DIR}")
    endif()
elseif(DS_GAME_DIR)
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> $<TARGET_PDB_FILE:dinput8> "${DS_GAME_DIR}"
        COMMENT "Deploying dinput8.dll to ${DS_GAME_DIR}")
endif()
