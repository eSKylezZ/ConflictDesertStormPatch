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
set(PSE_GAME "" CACHE STRING "Build another game's standalone patch from games/<id> instead of DesertStormFix (ds2, cv, cgs, tge)")
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
# Distribution build: no log file, no debug output, no PDB. Packages games/ds1/dist/ + a zip.
option(DS_DIST "Build the release package (no diagnostics)" OFF)

# Lua 5.4.7 (MIT, third_party/lua): mission scripts (games/ds1/plugins/missions/scripthost.cpp; dsfix.dll's dev server). Library sources only.
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS third_party/lua/*.c)
add_library(lua_static STATIC ${LUA_SOURCES})
target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)

# ds_ship_target(<target> <description> <file name>): options for every binary that ships (shim, loader, game layer,
# dsfix + its plugins). Dev builds always get a PDB, in every configuration (plain Release has no /Zi or /DEBUG by
# default, so a deploy had no PDB to copy); /DEBUG turns /OPT:REF and /OPT:ICF off - put back for Release
# (RelWithDebInfo links /INCREMENTAL, which /OPT:ICF would override with a warning). Release package (DS_DIST): no PDB,
# no build timestamp (/Brepro: a content hash instead, reproducible builds), no Rich header (toolchain fingerprint).
# Plus a version resource (games/ds1/src/version.rc.in) naming the file.
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
    configure_file(${CMAKE_SOURCE_DIR}/games/ds1/src/version.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
    target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
endfunction()

enable_testing()
# Pivotal Script Extender core (pse/): loader services + pse_tests.exe.
add_subdirectory(pse)

# HDR output shaders (games/ds1/src/render/hdr.hlsl) compiled by the Windows SDK's fxc into headers - no d3dcompiler at run time.
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
         /Fh ${CMAKE_BINARY_DIR}/generated/${file}.h ${CMAKE_SOURCE_DIR}/games/ds1/src/render/hdr.hlsl)
endforeach()
# Puddle reflections for the D3D9 renderers (games/ds1/src/render/water.hlsl), Shader Model 3.
foreach(entry "VSMain|vs_3_0|g_waterVs|water_vs" "PSMain|ps_3_0|g_waterPs|water_ps")
    string(REPLACE "|" ";" parts ${entry})
    list(GET parts 0 fn)
    list(GET parts 1 profile)
    list(GET parts 2 var)
    list(GET parts 3 file)
    list(APPEND WATER_SHADERS ${CMAKE_BINARY_DIR}/generated/${file}.h)
    list(APPEND WATER_SHADER_COMMANDS COMMAND ${FXC} /nologo /T ${profile} /E ${fn} /O3 /Vn ${var}
         /Fh ${CMAKE_BINARY_DIR}/generated/${file}.h ${CMAKE_SOURCE_DIR}/games/ds1/src/render/water.hlsl)
endforeach()
add_custom_command(OUTPUT ${WATER_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    ${WATER_SHADER_COMMANDS}
    DEPENDS games/ds1/src/render/water.hlsl
    COMMENT "Compiling water shaders")

add_custom_command(OUTPUT ${HDR_SHADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    ${HDR_SHADER_COMMANDS}
    DEPENDS games/ds1/src/render/hdr.hlsl
    COMMENT "Compiling HDR shaders")

# PSE\Plugins\dsfix.dll: DesertStormFix itself, a PSE host plugin (docs/sdk/layout.md). pse_add_plugin names it after
# the manifest (games/ds1/src/dsfix.pse) and puts it in pse_plugins/<config>/ next to the other plugins.
include(${CMAKE_SOURCE_DIR}/pse/cmake/PSEPlugin.cmake)
pse_add_plugin(dsfix MANIFEST games/ds1/src/dsfix.pse SOURCES
    games/ds1/src/dllmain.cpp
    games/ds1/src/core/log.cpp
    games/ds1/src/core/patch.cpp
    games/ds1/src/core/pse_host.cpp
    games/ds1/src/core/pse_saves.cpp games/ds1/src/core/pse_saves_format.cpp
    games/ds1/src/core/settings.cpp
    games/ds1/src/core/gamepad.cpp
    games/ds1/src/render/d3d8to9.cpp
    games/ds1/src/render/shader8.cpp
    games/ds1/src/render/presenter.cpp
    games/ds1/src/render/water.cpp
    ${HDR_SHADERS}
    ${WATER_SHADERS}
    games/ds1/src/features/controller.cpp
    games/ds1/src/features/timing.cpp
    games/ds1/src/features/framecap.cpp
    games/ds1/src/features/launcher.cpp
    games/ds1/src/features/launcher_pad.cpp
    games/ds1/src/features/ingame_input.cpp
    games/ds1/src/features/splitscreen.cpp
    games/ds1/src/features/dev.cpp
    games/ds1/src/features/campaigns.cpp
    games/ds1/src/features/graphics.cpp
    games/ds1/src/features/coop.cpp
    games/ds1/src/features/overlay.cpp
    games/ds1/src/features/barcolour.cpp
    games/ds1/src/features/padlayout.cpp
    games/ds1/src/features/controls.cpp
    games/ds1/src/features/padio.cpp
    games/ds1/src/features/hotplug.cpp
    games/ds1/src/features/multikbm.cpp
    games/ds1/src/features/services.cpp
    games/ds1/src/features/overlay_iface.cpp
    games/ds1/src/features/host_iface.cpp
    games/ds1/src/features/plugin_links.cpp
    games/ds1/src/features/skinapply.cpp
    games/ds1/src/features/loadout.cpp
    games/ds1/src/features/diagnostics.cpp
    games/ds1/src/features/memory.cpp
    games/ds1/src/features/console.cpp
    games/ds1/src/features/framerec.cpp
    games/ds1/src/features/renderdiff.cpp
    games/ds1/src/features/customise.cpp
    games/ds1/src/features/gear.cpp
    games/ds1/src/features/vehicles.cpp
    games/ds1/src/features/mods.cpp
    games/ds1/src/features/assetstore.cpp
    games/ds1/src/features/weaponicons.cpp
    games/ds1/src/features/overview.cpp
    games/ds1/src/features/versus.cpp
    games/ds1/src/features/modsmenu.cpp
    games/ds1/src/features/fixmenu.cpp
    games/ds1/src/features/videomenu.cpp
    games/ds1/src/features/pluginsmenu.cpp
    games/ds1/src/features/pseui.cpp
    games/ds1/src/core/pse_ui_model.cpp
    games/ds1/src/features/devserver.cpp
    games/ds1/src/features/originals.cpp
    games/ds1/src/game_icons.rc
)
target_include_directories(dsfix PRIVATE games/ds1/src games/ds1/plugins ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/pse/loader
    ${CMAKE_SOURCE_DIR}/pse/hostlink ${CMAKE_SOURCE_DIR}/pse/games/ds1)
target_compile_definitions(dsfix PRIVATE DS_VERSION="${DS_VERSION_FULL}")
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
# pse_hostlink, not pse_core: the loader services live in pse_loader.dll (one copy per process).
target_link_libraries(dsfix PRIVATE pse_hostlink pse_build_lib lua_static winmm msimg32 dxguid dinput8.lib ole32 shell32 shlwapi windowscodecs ws2_32 iphlpapi)
target_link_options(dsfix PRIVATE /SAFESEH:NO)
ds_ship_target(dsfix "DesertStormFix - Conflict: Desert Storm enhancement patch (PSE host plugin)" dsfix.dll)

# DesertStormFix-Patcher.exe: turns the Large Address Aware flag of DesertStorm.exe on / off (games/ds1/src/patcher).
set(PATCHER_MANIFEST "${CMAKE_SOURCE_DIR}/games/ds1/src/patcher/patcher.manifest")
set(PATCHER_DESCRIPTION "DesertStormFix Patcher (4 GB memory for Conflict: Desert Storm)")
set(PATCHER_NAME DesertStormFix-Patcher)
set(PATCHER_PRODUCT DesertStormFix)
configure_file(games/ds1/src/patcher/patcher.rc.in ${CMAKE_BINARY_DIR}/patcher.rc @ONLY)
add_executable(patcher WIN32 games/ds1/src/patcher/patcher.cpp ${CMAKE_BINARY_DIR}/patcher.rc)
set_target_properties(patcher PROPERTIES OUTPUT_NAME DesertStormFix-Patcher)
set_property(SOURCE ${CMAKE_BINARY_DIR}/patcher.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/games/ds1/src/patcher/patcher.manifest")
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
pse_add_plugin(dsfix_discord SOURCES games/ds1/plugins/discord/discord.cpp MANIFEST games/ds1/plugins/discord/dsfix.discord.pse)
ds_ship_target(dsfix_discord "DesertStormFix - Discord status (PSE plugin)" dsfix.discord.dll)
set(DSFIX_PLUGINS dsfix_discord)

# The dsfix kit (games/ds1/plugins/kit): DesertStormFix features moved out of dsfix.dll into their own plugin DLLs keep
# their core/patch.cpp + dslog code, on the public SDK (pse.log / pse.hooks / pse.settings - kit.h). One static copy per
# plugin; nothing of dsfix.dll or the loader's internals is linked.
add_library(dsfix_kit STATIC games/ds1/plugins/kit/kit.cpp games/ds1/plugins/kit/overlay_client.cpp games/ds1/src/core/patch.cpp pse/loader/x86len.cpp)
target_include_directories(dsfix_kit PUBLIC games/ds1/src games/ds1/plugins ${PSE_SDK_INCLUDE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/pse/loader)
target_compile_definitions(dsfix_kit PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DS_VERSION="${DS_VERSION_FULL}" $<$<BOOL:${DS_DIST}>:DS_DIST>)
target_compile_options(dsfix_kit PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi> $<$<BOOL:${DS_DIST}>:/Brepro>)

# dsfix_add_plugin(<target> <folder under games/ds1/plugins> <description> [extra sources]): a split-out feature
# plugin dsfix.<name> (manifest <folder>/dsfix.<name>.pse) on the kit, shipped with the others in PSE\Plugins.
function(dsfix_add_plugin target folder description)
    file(GLOB manifest "${CMAKE_SOURCE_DIR}/games/ds1/plugins/${folder}/*.pse")
    file(GLOB sources "${CMAKE_SOURCE_DIR}/games/ds1/plugins/${folder}/*.cpp")
    # the kit's host client (dsfix.dll's functions through dsfix.host) is compiled per plugin: its own_functions.h
    # (plugins/<folder>/generated, from its .spec) leaves out what the plugin implements itself
    pse_add_plugin(${target} SOURCES ${sources} ${CMAKE_SOURCE_DIR}/games/ds1/plugins/kit/host_client.cpp ${ARGN}
                   MANIFEST ${manifest})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/games/ds1/plugins/${folder}/generated)
    target_compile_options(${target} PRIVATE /FS)   # host_client.cpp: compiled in many targets at once
    target_link_libraries(${target} PRIVATE dsfix_kit)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_link_options(${target} PRIVATE /SAFESEH:NO)
    get_filename_component(stem "${manifest}" NAME_WLE)
    ds_ship_target(${target} "DesertStormFix - ${description} (PSE plugin)" ${stem}.dll)
    set(DSFIX_PLUGINS ${DSFIX_PLUGINS} ${target} PARENT_SCOPE)
endfunction()
dsfix_add_plugin(dsfix_shadows shadows "Shadow fix")
dsfix_add_plugin(dsfix_audio audio "Audio while aiming")
dsfix_add_plugin(dsfix_widescreen widescreen "Widescreen resolutions")
dsfix_add_plugin(dsfix_textfit textfit "Menu text fitting")
dsfix_add_plugin(dsfix_mouse mouse "Mouse acceleration")
dsfix_add_plugin(dsfix_menus menus "Menu fixes")
dsfix_add_plugin(dsfix_units units "Unit / vehicle tables")
dsfix_add_plugin(dsfix_limits limits "Engine limits")
# on interface dsfix.services (games/ds1/src/core/dsfix_services.h, published by dsfix.dll)
dsfix_add_plugin(dsfix_difficulty difficulty "VERY HARD / VERY EASY")
dsfix_add_plugin(dsfix_jpfixes jpfixes "Japanese-release fixes")
dsfix_add_plugin(dsfix_handling handling "Vehicle handling")
dsfix_add_plugin(dsfix_tooltips tooltips "Inventory tooltips")
dsfix_add_plugin(dsfix_rumble rumble "Weapon rumble")
dsfix_add_plugin(dsfix_bodies bodies "Soldier bodies from mods")
dsfix_add_plugin(dsfix_followers followers "Playable followers")
dsfix_add_plugin(dsfix_hud hud "HUD / menu scaling")
dsfix_add_plugin(dsfix_weather weather "Weather + footprints" games/ds1/plugins/weather/weather_sounds.rc)
# the sounds (assets/weather-elements, built in as RCDATA) - a changed .adpcm rebuilds the resource
target_include_directories(dsfix_weather PRIVATE ${CMAKE_SOURCE_DIR}/assets)
file(GLOB DSFIX_WEATHER_SOUNDS ${CMAKE_SOURCE_DIR}/assets/weather-elements/*.adpcm)
set_source_files_properties(games/ds1/plugins/weather/weather_sounds.rc PROPERTIES OBJECT_DEPENDS "${DSFIX_WEATHER_SOUNDS}")
dsfix_add_plugin(dsfix_timeofday timeofday "Time of day")
dsfix_add_plugin(dsfix_voices voices "Squad voices")
dsfix_add_plugin(dsfix_fontsharp fontsharp "Sharper text")
dsfix_add_plugin(dsfix_developer developer "Developer page + debug keys")
dsfix_add_plugin(dsfix_cinematics cinematics "Intro / cutscene skip")
dsfix_add_plugin(dsfix_display display "Display mode")
dsfix_add_plugin(dsfix_photo photo "Photo mode, free camera, cheats menu" games/ds1/plugins/photo/photo.rc)
set_source_files_properties(games/ds1/plugins/photo/photo.rc PROPERTIES OBJECT_DEPENDS
                            ${CMAKE_SOURCE_DIR}/games/ds1/src/generated/photo_frames.png)
target_link_libraries(dsfix_photo PRIVATE shlwapi)
# online co-op: compiles the online check's pure half (pse/loader/compat.cpp; LocalPlugins() comes from dsfix.dll)
dsfix_add_plugin(dsfix_online online "Online co-op" pse/loader/compat.cpp)
target_include_directories(dsfix_online PRIVATE ${CMAKE_SOURCE_DIR}/pse/loader)
target_link_libraries(dsfix_online PRIVATE bcrypt ws2_32 iphlpapi)
# mission scripts: its own Lua (lua_static) and the mission translator (pse_build_lib = pse_m2l)
dsfix_add_plugin(dsfix_missions missions "Mission scripts")
target_include_directories(dsfix_missions PRIVATE ${CMAKE_SOURCE_DIR}/pse/loader)
target_link_libraries(dsfix_missions PRIVATE lua_static pse_build_lib winmm)

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
string(APPEND DS_TREE "@$<TARGET_FILE:dsfix>|PSE/Plugins/dsfix.dll@${CMAKE_SOURCE_DIR}/games/ds1/src/dsfix.pse|PSE/Plugins/dsfix.pse")
set(DS_TREE_DEPENDS ${CMAKE_SOURCE_DIR}/games/ds1/src/dsfix.pse)
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
# The list goes to assemble.cmake in a file: on the command line it outgrew cmd.exe's 8191 characters.
set(DS_TREE_LIST ${CMAKE_BINARY_DIR}/tree-files-$<CONFIG>.txt)
file(GENERATE OUTPUT ${DS_TREE_LIST} CONTENT "${DS_TREE}")

if(DS_DIST)
    set(DIST_DIR ${CMAKE_BINARY_DIR}/package)
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStormFix-${DS_VERSION_FULL}.zip)
    set(DIST_README "${CMAKE_SOURCE_DIR}/games/ds1/dist/DesertStormFix-README.txt")
    set(DIST_INSTALL "")
    set(DIST_COMMENT "Packaging ${DIST_ZIP}")
    set(DIST_LIST ${CMAKE_BINARY_DIR}/package-files-$<CONFIG>.txt)
    file(GENERATE OUTPUT ${DIST_LIST} CONTENT "${DS_TREE}@${DIST_README}|DesertStormFix-README.txt")
    if(DS_GAME_DIR)
        set(DIST_INSTALL COMMAND ${CMAKE_COMMAND} -DDEST=${DS_GAME_DIR} -DFILES_FROM=${DIST_LIST} -P ${DS_ASSEMBLE})
        string(APPEND DIST_COMMENT " and installing it into ${DS_GAME_DIR}")
    endif()
    add_custom_command(OUTPUT "${DIST_ZIP}"
        COMMAND ${CMAKE_COMMAND} -DDEST=${DIST_DIR} -DCLEAN=1 -DFILES_FROM=${DIST_LIST} -P ${DS_ASSEMBLE}
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
        set(TREE_DEPLOY COMMAND ${CMAKE_COMMAND} -DDEST=${DS_GAME_DIR} -DFILES_FROM=${DS_TREE_LIST} -P ${DS_ASSEMBLE})
        string(APPEND TREE_COMMENT " + deploying dinput8.dll and PSE\\ to ${DS_GAME_DIR}")
    endif()
    add_custom_command(OUTPUT ${TREE_STAMP}
        COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> -DFILES_FROM=${DS_TREE_LIST} -P ${DS_ASSEMBLE}
        ${TREE_DEPLOY}
        COMMAND ${CMAKE_COMMAND} -E touch ${TREE_STAMP}
        DEPENDS ${DS_SHIPPED_TARGETS} ${DS_TREE_DEPENDS} ${DS_ASSEMBLE}
        COMMENT "${TREE_COMMENT}"
        VERBATIM)
    add_custom_target(pse_tree ALL DEPENDS ${TREE_STAMP})
endif()
