cmake_minimum_required(VERSION 3.21)
project(DesertStormFix VERSION 3.3.0 LANGUAGES C CXX RC)
# Pre-release tag shown with the version (DLL, menus, launcher, zip name); "" for a release. The Windows file version
# stays numeric (3.2.1.0).
set(DS_VERSION_SUFFIX "-Alpha")
set(DS_VERSION_FULL "${PROJECT_VERSION}${DS_VERSION_SUFFIX}")

# ---- Other games' standalone patches (games/<id>, 2026-10-19) --------------------------------------------------------
# PSE_GAME = ds2 / cv / cgs builds ONLY that game's own patch (games/<id>/CMakeLists.txt: dinput8.dll, DS2 also ds2fix)
# in this binary dir - nothing of DesertStormFix / PSE is configured, so no target names clash. Root presets ds2*, cv*,
# cgs*. Outputs land where a standalone build of games/<id> puts them (<bin>/<config>/dinput8.dll, <bin>/package).
# Each games/<id> also still builds on its own: cmake -S games/<id> --preset x86 (its own CMakePresets.json).
set(PSE_GAME "" CACHE STRING "Build another game's standalone patch from games/<id> instead of DesertStormFix (ds2, cv, cgs)")
if(PSE_GAME)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/games/${PSE_GAME}/CMakeLists.txt")
        message(FATAL_ERROR "PSE_GAME=${PSE_GAME}: no games/${PSE_GAME}/CMakeLists.txt")
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_subdirectory(games/${PSE_GAME} ${CMAKE_BINARY_DIR}/game)
    return()
endif()

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

# Lua 5.4.7 (MIT, third_party/lua): mission scripts (src/features/scripthost.cpp). Library sources only.
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS third_party/lua/*.c)
add_library(lua_static STATIC ${LUA_SOURCES})
target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)

# ds_ship_target(<target> <description> <file name>): options for every binary that ships (shim, loader, game layer,
# dsfix + its plugins). Dev builds always get a PDB, in every configuration (plain Release has no /Zi or /DEBUG by
# default, so a deploy had no PDB to copy); /DEBUG turns /OPT:REF and /OPT:ICF off - put back for Release
# (RelWithDebInfo links /INCREMENTAL, which /OPT:ICF would override with a warning). Release package (DS_DIST): no PDB,
# no build timestamp (/Brepro: a content hash instead, reproducible builds), no Rich header (toolchain fingerprint).
# Plus a version resource (src/version.rc.in) naming the file.
function(ds_ship_target t description filename)
    target_compile_definitions(${t} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        $<$<BOOL:${DS_DIST}>:DS_DIST>)
    target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    if(DS_DIST)
        target_compile_options(${t} PRIVATE /Brepro)
        target_link_options(${t} PRIVATE /DEBUG:NONE /Brepro /EMITTOOLVERSIONINFO:NO)
    else()
        target_compile_options(${t} PRIVATE /Zi)
        target_link_options(${t} PRIVATE /DEBUG $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF>)
    endif()
    set(DS_RC_DESCRIPTION "${description}")
    set(DS_RC_FILENAME "${filename}")
    get_filename_component(DS_RC_INTERNAL "${filename}" NAME_WE)
    configure_file(${CMAKE_SOURCE_DIR}/src/version.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
    target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
endfunction()

enable_testing()
# Pivotal Script Extender core (pse/): loader services + pse_tests.exe.
add_subdirectory(pse)

# HDR output shaders (src/render/hdr.hlsl) compiled by the Windows SDK's fxc into headers - no d3dcompiler at run time.
find_program(FXC fxc PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.0.26100.0/x64"
             "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(HDR_SHADER_ENTRIES "VSMain|vs_5_0|g_hdrVs|hdr_vs" "PSMain|ps_5_0|g_hdrPs|hdr_ps" "PSMainSdr|ps_5_0|g_hdrPsSdr|hdr_ps_sdr"
    "PSPrefilter|ps_5_0|g_bloomPre|bloom_pre" "PSDown|ps_5_0|g_bloomDown|bloom_down" "PSUp|ps_5_0|g_bloomUp|bloom_up")
set(HDR_SHADERS "")
set(HDR_SHADER_COMMANDS "")
foreach(entry ${HDR_SHADER_ENTRIES})
    string(REPLACE "|" ";" parts ${entry})
    list(GET parts 0 fn)
    list(GET parts 1 profile)
    list(GET parts 2 var)
    list(GET parts 3 file)
    list(APPEND HDR_SHADERS ${CMAKE_BINARY_DIR}/generated/${file}.h)
    list(APPEND HDR_SHADER_COMMANDS COMMAND ${FXC} /nologo /T ${profile} /E ${fn} /O3 /Vn ${var}
         /Fh ${CMAKE_BINARY_DIR}/generated/${file}.h ${CMAKE_SOURCE_DIR}/src/render/hdr.hlsl)
endforeach()
# Puddle reflections for the D3D9 renderers (src/render/water.hlsl), Shader Model 3.
foreach(entry "VSMain|vs_3_0|g_waterVs|water_vs" "PSMain|ps_3_0|g_waterPs|water_ps")
    string(REPLACE "|" ";" parts ${entry})
    list(GET parts 0 fn)
    list(GET parts 1 profile)
    list(GET parts 2 var)
    list(GET parts 3 file)
    list(APPEND WATER_SHADERS ${CMAKE_BINARY_DIR}/generated/${file}.h)
    list(APPEND WATER_SHADER_COMMANDS COMMAND ${FXC} /nologo /T ${profile} /E ${fn} /O3 /Vn ${var}
         /Fh ${CMAKE_BINARY_DIR}/generated/${file}.h ${CMAKE_SOURCE_DIR}/src/render/water.hlsl)
endforeach()
add_custom_command(OUTPUT ${WATER_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    ${WATER_SHADER_COMMANDS}
    DEPENDS src/render/water.hlsl
    COMMENT "Compiling water shaders")

add_custom_command(OUTPUT ${HDR_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    ${HDR_SHADER_COMMANDS}
    DEPENDS src/render/hdr.hlsl
    COMMENT "Compiling HDR shaders")

# PSE\Plugins\dsfix.dll: DesertStormFix itself, a PSE host plugin (docs/sdk/layout.md). pse_add_plugin names it after
# the manifest (src/dsfix.pse) and puts it in pse_plugins/<config>/ next to the other plugins.
include(${CMAKE_SOURCE_DIR}/pse/cmake/PSEPlugin.cmake)
pse_add_plugin(dsfix MANIFEST src/dsfix.pse SOURCES
    src/dllmain.cpp
    src/core/log.cpp
    src/core/patch.cpp
    src/core/pse_host.cpp
    src/core/pse_saves.cpp src/core/pse_saves_format.cpp
    src/core/settings.cpp
    src/core/gamepad.cpp
    src/render/d3d8to9.cpp
    src/render/shader8.cpp
    src/render/presenter.cpp
    src/render/water.cpp
    ${HDR_SHADERS}
    ${WATER_SHADERS}
    src/core/net.cpp
    src/features/onlinecompat.cpp
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
    src/features/scripthost.cpp
    src/features/movie.cpp
    src/features/campaigns.cpp
    src/features/display.cpp
    src/features/cinematics.cpp
    src/features/graphics.cpp
    src/features/coop.cpp
    src/features/overlay.cpp
    src/features/fontsharp.cpp
    src/features/barcolour.cpp
    src/features/padlayout.cpp
    src/features/controls.cpp
    src/features/padio.cpp
    src/features/hotplug.cpp
    src/features/mouse.cpp
    src/features/menus.cpp
    src/features/followers.cpp
    src/features/difficulty.cpp
    src/features/jpfixes.cpp
    src/features/units.cpp
    src/features/handling.cpp
    src/features/limits.cpp
    src/features/tooltips.cpp
    src/features/rumble.cpp
    src/features/audio.cpp
    src/features/shadows.cpp
    src/features/loadout.cpp
    src/features/diagnostics.cpp
    src/features/memory.cpp
    src/features/console.cpp
    src/features/framerec.cpp
    src/features/authority.cpp
    src/features/renderdiff.cpp
    src/features/customise.cpp
    src/features/gear.cpp
    src/features/vehicles.cpp
    src/features/mods.cpp
    src/features/assetstore.cpp
    src/features/weaponicons.cpp
    src/features/bodies.cpp
    src/features/timeofday.cpp
    src/features/footprints.cpp
    src/features/weather.cpp
    src/features/weathersound.cpp
    src/features/voices.cpp
    src/features/overview.cpp
    src/features/versus.cpp
    src/features/cheats.cpp
    src/features/freecam.cpp
    src/features/developer.cpp
    src/features/modsmenu.cpp
    src/features/fixmenu.cpp
    src/features/videomenu.cpp
    src/features/pluginsmenu.cpp
    src/features/pseui.cpp
    src/core/pse_ui_model.cpp
    src/features/textfit.cpp
    src/features/pausemenu.cpp
    src/features/photomode.cpp
    src/features/photoframes.cpp
    src/features/online.cpp
    src/features/determinism.cpp
    src/features/lockstep.cpp
    src/features/hostsync.cpp
    src/features/devserver.cpp
    src/features/missionrt.cpp src/features/apitrace.cpp src/features/missionsource.cpp src/features/originals.cpp
    src/game_icons.rc
)
target_include_directories(dsfix PRIVATE src ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/pse/loader
    ${CMAKE_SOURCE_DIR}/pse/hostlink ${CMAKE_SOURCE_DIR}/pse/games/ds1)
target_compile_definitions(dsfix PRIVATE DS_VERSION="${DS_VERSION_FULL}")
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
# pse_hostlink, not pse_core: the loader services live in pse_loader.dll (one copy per process).
target_link_libraries(dsfix PRIVATE pse_hostlink pse_build_lib lua_static winmm msimg32 dxguid dinput8.lib ole32 shell32 shlwapi windowscodecs ws2_32 iphlpapi)
target_link_options(dsfix PRIVATE /SAFESEH:NO)
ds_ship_target(dsfix "DesertStormFix - Conflict: Desert Storm enhancement patch (PSE host plugin)" dsfix.dll)

# DesertStormFix-Patcher.exe: turns the Large Address Aware flag of DesertStorm.exe on / off (src/patcher).
configure_file(src/patcher/patcher.rc.in ${CMAKE_BINARY_DIR}/patcher.rc @ONLY)
add_executable(patcher WIN32 src/patcher/patcher.cpp ${CMAKE_BINARY_DIR}/patcher.rc)
set_target_properties(patcher PROPERTIES OUTPUT_NAME DesertStormFix-Patcher)
set_property(SOURCE ${CMAKE_BINARY_DIR}/patcher.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/patcher/patcher.manifest")
target_compile_definitions(patcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(patcher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(patcher PRIVATE bcrypt comctl32 shell32 ole32)
target_link_options(patcher PRIVATE /MANIFEST:NO $<$<BOOL:${DS_DIST}>:/DEBUG:NONE /Brepro /EMITTOOLVERSIONINFO:NO>)
if(DS_DIST)
    target_compile_options(patcher PRIVATE /Brepro)
endif()
add_dependencies(dsfix patcher)   # shipped next to it (package / deploy below)

# DesertStormFix's external PSE plugins (plugins/<name>/, docs/sdk/ds1-plugins.md): built from the SDK only
# (pse/include + pse_add_plugin), in every build - the release package ships them in PSE\Plugins.
pse_add_plugin(dsfix_discord SOURCES plugins/discord/discord.cpp MANIFEST plugins/discord/dsfix.discord.pse)
ds_ship_target(dsfix_discord "DesertStormFix - Discord status (PSE plugin)" dsfix.discord.dll)
set(DSFIX_PLUGINS dsfix_discord)

# ---- the install tree (docs/sdk/layout.md) ---------------------------------------------------------------------------
#   <game>\dinput8.dll                       shim (pse_shim)
#   <game>\DesertStormFix-Patcher.exe        4 GB patcher
#   <game>\PSE\pse_loader.dll                loader
#   <game>\PSE\Games\pse_ds1.dll             DS1 game layer
#   <game>\PSE\Plugins\dsfix.dll + .pse      DesertStormFix (host plugin)
#   <game>\PSE\Plugins\dsfix.<name>.dll + .pse   DesertStormFix's other plugins
# Dev builds: the same tree with PDBs in ${CMAKE_BINARY_DIR}/tree/<config>/ (the live test runner deploys it), and
# into DS_GAME_DIR when set. Release package (DS_DIST): package/ without PDBs + the readme, zipped, installed into
# DS_GAME_DIR when set. Files are copied only when they differ (pse/cmake/assemble.cmake).
set(DS_SHIPPED_TARGETS pse_shim pse_loader pse_ds1 dsfix ${DSFIX_PLUGINS} patcher)
set(DS_TREE "$<TARGET_FILE:pse_shim>|dinput8.dll@$<TARGET_FILE:patcher>|DesertStormFix-Patcher.exe")
string(APPEND DS_TREE "@$<TARGET_FILE:pse_loader>|PSE/pse_loader.dll@$<TARGET_FILE:pse_ds1>|PSE/Games/pse_ds1.dll")
string(APPEND DS_TREE "@$<TARGET_FILE:dsfix>|PSE/Plugins/dsfix.dll@${CMAKE_SOURCE_DIR}/src/dsfix.pse|PSE/Plugins/dsfix.pse")
set(DS_TREE_DEPENDS ${CMAKE_SOURCE_DIR}/src/dsfix.pse)
# the records schema (how the game's tables read as records; mods' records\*.lua edits need it - mods.cpp ApplyRecords)
string(APPEND DS_TREE "@${CMAKE_SOURCE_DIR}/pse/records/ds1/schema.lua|PSE/Records/ds1/schema.lua")
list(APPEND DS_TREE_DEPENDS ${CMAKE_SOURCE_DIR}/pse/records/ds1/schema.lua)
foreach(t IN LISTS DSFIX_PLUGINS)
    get_target_property(stem ${t} OUTPUT_NAME)
    get_target_property(manifest ${t} PSE_MANIFEST)
    string(APPEND DS_TREE "@$<TARGET_FILE:${t}>|PSE/Plugins/${stem}.dll@${manifest}|PSE/Plugins/${stem}.pse")
    list(APPEND DS_TREE_DEPENDS ${manifest})
endforeach()
if(NOT DS_DIST)
    string(APPEND DS_TREE "@?$<TARGET_PDB_FILE:pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:pse_loader>|PSE/pse_loader.pdb")
    string(APPEND DS_TREE "@?$<TARGET_PDB_FILE:pse_ds1>|PSE/Games/pse_ds1.pdb@?$<TARGET_PDB_FILE:dsfix>|PSE/Plugins/dsfix.pdb")
    foreach(t IN LISTS DSFIX_PLUGINS)
        get_target_property(stem ${t} OUTPUT_NAME)
        string(APPEND DS_TREE "@?$<TARGET_PDB_FILE:${t}>|PSE/Plugins/${stem}.pdb")
    endforeach()
endif()
set(DS_ASSEMBLE ${CMAKE_SOURCE_DIR}/pse/cmake/assemble.cmake)

if(DS_DIST)
    set(DIST_DIR ${CMAKE_BINARY_DIR}/package)
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStormFix-${DS_VERSION_FULL}.zip)
    set(DIST_README "${CMAKE_SOURCE_DIR}/dist/DesertStormFix-README.txt")
    set(DIST_INSTALL "")
    set(DIST_COMMENT "Packaging ${DIST_ZIP}")
    if(DS_GAME_DIR)
        set(DIST_INSTALL COMMAND ${CMAKE_COMMAND} -DDEST=${DS_GAME_DIR} "-DFILES=${DS_TREE}@${DIST_README}|DesertStormFix-README.txt" -P ${DS_ASSEMBLE})
        string(APPEND DIST_COMMENT " and installing it into ${DS_GAME_DIR}")
    endif()
    add_custom_command(OUTPUT "${DIST_ZIP}"
        COMMAND ${CMAKE_COMMAND} -DDEST=${DIST_DIR} -DCLEAN=1 "-DFILES=${DS_TREE}@${DIST_README}|DesertStormFix-README.txt" -P ${DS_ASSEMBLE}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_ZIP}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${DIST_DIR}" ${CMAKE_COMMAND} -E tar cf "${DIST_ZIP}" --format=zip .
        ${DIST_INSTALL}
        DEPENDS ${DS_SHIPPED_TARGETS} ${DS_TREE_DEPENDS} "${DIST_README}" ${DS_ASSEMBLE}
        COMMENT "${DIST_COMMENT}"
        VERBATIM)
    add_custom_target(package_zip ALL DEPENDS "${DIST_ZIP}")
else()
    set(TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
    set(TREE_DEPLOY "")
    set(TREE_COMMENT "PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>")
    if(DS_GAME_DIR)
        set(TREE_DEPLOY COMMAND ${CMAKE_COMMAND} -DDEST=${DS_GAME_DIR} "-DFILES=${DS_TREE}" -P ${DS_ASSEMBLE})
        string(APPEND TREE_COMMENT " + deploying dinput8.dll and PSE\\ to ${DS_GAME_DIR}")
    endif()
    add_custom_command(OUTPUT ${TREE_STAMP}
        COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> "-DFILES=${DS_TREE}" -P ${DS_ASSEMBLE}
        ${TREE_DEPLOY}
        COMMAND ${CMAKE_COMMAND} -E touch ${TREE_STAMP}
        DEPENDS ${DS_SHIPPED_TARGETS} ${DS_TREE_DEPENDS} ${DS_ASSEMBLE}
        COMMENT "${TREE_COMMENT}"
        VERBATIM)
    add_custom_target(pse_tree ALL DEPENDS ${TREE_STAMP})
endif()
