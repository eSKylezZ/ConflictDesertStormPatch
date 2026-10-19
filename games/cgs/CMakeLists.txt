cmake_minimum_required(VERSION 3.21)
project(GlobalStormFix VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ConflictGlobal.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# Folder containing ConflictGlobal.exe; when set, every build copies the output there.
set(DS_GAME_DIR "" CACHE PATH "Conflict: Global Storm install folder (optional deploy target)")
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
    src/features/devbg.cpp
    src/features/widescreen.cpp
    src/features/hud.cpp
    src/features/graphics.cpp
    src/features/frame.cpp
    ${CMAKE_BINARY_DIR}/version.rc
)
target_include_directories(dinput8 PRIVATE src)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DS_VERSION="${PROJECT_VERSION}" $<$<BOOL:${DS_DIST}>:DS_DIST>)
target_compile_options(dinput8 PRIVATE /W4 /permissive- /utf-8)
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
target_link_libraries(dinput8 PRIVATE winmm dxguid dinput8.lib ole32)
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
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/GlobalStormFix-${PROJECT_VERSION}.zip)
    set_property(TARGET dinput8 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dist/GlobalStormFix-README.txt")
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${CMAKE_CURRENT_SOURCE_DIR}/dist/GlobalStormFix-README.txt" "${DIST_DIR}"
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
