cmake_minimum_required(VERSION 3.21)
project(DesertStorm2Fix VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "CDS II.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# Folder containing CDS II.exe; when set, every build copies the output there.
set(DS_GAME_DIR "" CACHE PATH "Conflict: Desert Storm II install folder (optional deploy target)")
# Distribution build: no log file, no debug output, no PDB. Packages dist/ + a zip.
option(DS_DIST "Build the release package (no diagnostics)" OFF)

configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

# Features ported from DesertStormFix (games/ds1-reference/ds1-features) are added here once their addresses are mapped
# (PORTING.md).
add_library(dinput8 SHARED
    src/dllmain.cpp
    src/dinput8.def
    src/core/log.cpp
    src/core/patch.cpp
    src/core/settings.cpp
    src/core/gamepad.cpp
    src/features/gameexe.cpp
    src/features/widescreen.cpp
    src/features/devbg.cpp
    src/features/hud.cpp
    src/features/graphics.cpp
    src/features/frontend.cpp
    src/features/fontsharp.cpp
    src/features/cinematics.cpp
    src/features/menus.cpp
    src/features/splitscreen.cpp
    src/features/coop.cpp
    ${CMAKE_BINARY_DIR}/version.rc
)
target_include_directories(dinput8 PRIVATE src)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DS_VERSION="${PROJECT_VERSION}" $<$<BOOL:${DS_DIST}>:DS_DIST>)
target_compile_options(dinput8 PRIVATE /W4 /permissive- /utf-8)
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
target_link_libraries(dinput8 PRIVATE winmm dxguid dinput8.lib ole32 d3d9)
target_link_options(dinput8 PRIVATE /SAFESEH:NO $<$<BOOL:${DS_DIST}>:/DEBUG:NONE>)
# Non-dist builds always get a PDB (see DesertStormFix's CMakeLists for the /OPT notes).
if(NOT DS_DIST)
    target_compile_options(dinput8 PRIVATE /Zi)
    target_link_options(dinput8 PRIVATE /DEBUG $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF>)
endif()
if(DS_DIST)
    target_compile_options(dinput8 PRIVATE /Brepro)
    target_link_options(dinput8 PRIVATE /Brepro /EMITTOOLVERSIONINFO:NO)
endif()

if(DS_DIST)
    set(DIST_DIR ${CMAKE_BINARY_DIR}/package)
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStorm2Fix-${PROJECT_VERSION}.zip)
    set_property(TARGET dinput8 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dist/DesertStorm2Fix-README.txt")
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${CMAKE_CURRENT_SOURCE_DIR}/dist/DesertStorm2Fix-README.txt" "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${DIST_DIR}" ${CMAKE_COMMAND} -E tar cf "${DIST_ZIP}" --format=zip .
        COMMENT "Packaging ${DIST_ZIP}")
    if(DS_GAME_DIR)
        add_custom_command(TARGET dinput8 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIST_DIR}" "${DS_GAME_DIR}"
            COMMENT "Installing package into ${DS_GAME_DIR}")
    endif()
elseif(DS_GAME_DIR)
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${DS_GAME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:dinput8> "${DS_GAME_DIR}"
        COMMENT "Deploying dinput8.dll + .pdb to ${DS_GAME_DIR}")
endif()

# ---- DesertStorm2Fix as a PSE host plugin (SKELETON, 2026-10-19) -----------------------------------------------------
# ds2fix.dll + ds2fix.pse for <game>\PSE\Plugins (Pivotal Script Extender = this repo's pse/: shim dinput8.dll,
# PSE\pse_loader.dll, PSE\Games\pse_ds2.dll from pse/games/ds2). Same features as dinput8.dll; patches through the PSE
# broker (DesertStormFix's broker-backed src/core/patch.cpp at the repo root), pse:: calls through pse/hostlink. Off by
# default; the dinput8 target above is unchanged. Build in its own binary dir (no DS_GAME_DIR - deploying it replaces
# nothing here), from games/ds2:
#   cmake -S . -B build-pse -A Win32 -DDS2_PSE_PLUGIN=ON && cmake --build build-pse --config RelWithDebInfo --target ds2fix
# or from the repo root: cmake --preset ds2-pse && cmake --build --preset ds2-pse
option(DS2_PSE_PLUGIN "Also build ds2fix.dll, DesertStorm2Fix as a PSE host plugin" OFF)
set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, src/core/patch.cpp)")
if(DS2_PSE_PLUGIN)
    get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
    if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
        message(FATAL_ERROR "DS2_PSE_PLUGIN: PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
    endif()
    add_library(ds2fix SHARED
        src/pse/ds2fix.cpp src/pse/ds2fix.pse
        src/core/log.cpp src/core/settings.cpp src/core/gamepad.cpp
        src/features/gameexe.cpp src/features/widescreen.cpp src/features/devbg.cpp src/features/hud.cpp
        src/features/graphics.cpp src/features/frontend.cpp src/features/fontsharp.cpp src/features/cinematics.cpp
        src/features/menus.cpp src/features/splitscreen.cpp src/features/coop.cpp
        ${PSE_REPO_ABS}/src/core/patch.cpp
        ${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp ${PSE_REPO_ABS}/pse/loader/compat.cpp
        ${PSE_REPO_ABS}/pse/loader/x86len.cpp ${PSE_REPO_ABS}/pse/loader/settings_migrate.cpp
        ${PSE_REPO_ABS}/pse/loader/util.cpp)
    set_source_files_properties(src/pse/ds2fix.pse PROPERTIES HEADER_FILE_ONLY ON)
    # src/pse/shim first: its core/patch.h / core/pse_host.h replace this repo's for the host build.
    target_include_directories(ds2fix PRIVATE src/pse/shim src ${PSE_REPO_ABS}/pse/include ${PSE_REPO_ABS}/pse/loader
        ${PSE_REPO_ABS}/pse/hostlink)
    target_compile_definitions(ds2fix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS DS2_PSE_HOST
        DS_VERSION="${PROJECT_VERSION}" "DS2_PSE_PATCH_H=\"${PSE_REPO_ABS}/src/core/patch.h\""
        $<$<BOOL:${DS_DIST}>:DS_DIST>)
    target_compile_options(ds2fix PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(ds2fix PRIVATE winmm dxguid dinput8.lib ole32 d3d9 advapi32 bcrypt)
    target_link_options(ds2fix PRIVATE /SAFESEH:NO $<$<NOT:$<BOOL:${DS_DIST}>>:/DEBUG>)
    set_target_properties(ds2fix PROPERTIES OUTPUT_NAME ds2fix PREFIX "")
    add_custom_command(TARGET ds2fix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/pse/ds2fix.pse" "$<TARGET_FILE_DIR:ds2fix>"
        COMMENT "ds2fix.pse next to ds2fix.dll")
endif()
