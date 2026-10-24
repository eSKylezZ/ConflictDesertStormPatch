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

# The PSE build (DesertStorm2Fix as the host plugin ds2fix.dll + the PSE runtime, packaged; section at the end). With it
# on, the standalone dinput8.dll below still builds (until ds2fix has replaced it in tests), its package goes to
# package-standalone/ + DesertStorm2Fix-<ver>-standalone.zip and package/ + DesertStorm2Fix-<ver>.zip are the PSE ones.
option(DS2_PSE_PLUGIN "Also build ds2fix.dll, DesertStorm2Fix as a PSE host plugin, and its PSE install tree / package" OFF)

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
    src/features/hudfix.cpp
    src/features/graphics.cpp
    src/features/frontend.cpp
    src/features/fontsharp.cpp
    src/features/cinematics.cpp
    src/features/menus.cpp
    src/features/splitscreen.cpp
    src/features/coop.cpp
    src/features/mods.cpp
    src/features/modsmenu.cpp
    src/features/skins.cpp
    src/features/mods_levels.cpp
    src/features/mods_sound.cpp
    src/features/mods_tables.cpp
    src/features/weaponicons.cpp
    src/features/scripthost.cpp
    src/features/campaigns.cpp
    ${CMAKE_BINARY_DIR}/version.rc
)
target_include_directories(dinput8 PRIVATE src)
# Lua 5.4.7 (MIT, the repo root's third_party/lua): .lua mission scripts (scripthost.cpp). Library sources only.
set(DS2_LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/lua")
file(GLOB DS2_LUA_SOURCES CONFIGURE_DEPENDS ${DS2_LUA_DIR}/*.c)
add_library(ds2_lua STATIC ${DS2_LUA_SOURCES})
target_include_directories(ds2_lua PUBLIC ${DS2_LUA_DIR})
target_compile_definitions(ds2_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dinput8 PRIVATE ds2_lua)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DS_VERSION="${PROJECT_VERSION}" $<$<BOOL:${DS_DIST}>:DS_DIST>)
target_compile_options(dinput8 PRIVATE /W4 /permissive- /utf-8)
# dinput8.lib only for its static data (c_dfDIJoystick2); DirectInput8Create is called via the real DLL.
target_link_libraries(dinput8 PRIVATE winmm dxguid dinput8.lib ole32 d3d9 windowscodecs)
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
    if(DS2_PSE_PLUGIN)
        set(DIST_DIR ${CMAKE_BINARY_DIR}/package-standalone)
        set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStorm2Fix-${PROJECT_VERSION}-standalone.zip)
    else()
        set(DIST_DIR ${CMAKE_BINARY_DIR}/package)
        set(DIST_ZIP ${CMAKE_BINARY_DIR}/DesertStorm2Fix-${PROJECT_VERSION}.zip)
    endif()
    # The standalone build's own readme (dist/DesertStorm2Fix-README.txt describes the PSE package), named as before.
    set(DIST_README "${CMAKE_CURRENT_SOURCE_DIR}/dist/DesertStorm2Fix-standalone-README.txt")
    set_property(TARGET dinput8 APPEND PROPERTY LINK_DEPENDS "${DIST_README}")
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${DIST_README}" "${DIST_DIR}/DesertStorm2Fix-README.txt"
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


# ---- DesertStorm2Fix on the Pivotal Script Extender (PSE) --------------------------------------------------------------
# The DS2 product: the PSE install tree (docs/sdk/layout.md, docs/sdk/reference/game-ds2.md)
#   dinput8.dll                     the PSE shim (game-neutral, pse/shim)
#   PSE\pse_loader.dll              the loader (pse/loader + pse/loader_dll)
#   PSE\Games\pse_ds2.dll           the DS2 game layer (pse/games/ds2)
#   PSE\Plugins\ds2fix.dll + .pse   DesertStorm2Fix as the host plugin (src/pse/ds2fix.cpp)
#   PSE\Plugins\<name>.dll + .pse   every DS2 plugin in plugins/<name>/ (see below)
#   DesertStorm2Fix-Patcher.exe    4 GB patcher (repo root src/patcher, PATCHER_GAME_DS2)
# assembled in ${CMAKE_BINARY_DIR}/tree/<config>/ by every build (pse/cmake/assemble.cmake), copied into DS2_GAME_DIR
# when set (changed files only), and with DS_DIST packaged (no PDBs, + the readme) into package/ +
# DesertStorm2Fix-<ver>.zip. The shim, loader and layer are built here from the repo's pse/ sources (a PSE_GAME=ds2
# configure has no DS1 / PSE project), with the options of DS1's ds_ship_target. From games/ds2:
#   cmake -S . -B build-pse -A Win32 -DDS2_PSE_PLUGIN=ON && cmake --build build-pse --config RelWithDebInfo
# or from the repo root: cmake --preset ds2-pse && cmake --build --preset ds2-pse (release package: ds2-pse-dist).
set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, src/core/patch.cpp)")
set(DS2_GAME_DIR "" CACHE PATH "Conflict: Desert Storm II install folder: the PSE tree is copied there (replaces dinput8.dll)")
if(DS2_PSE_PLUGIN)
    get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
    if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
        message(FATAL_ERROR "DS2_PSE_PLUGIN: PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
    endif()
    if(DS2_GAME_DIR AND DS_GAME_DIR)
        message(FATAL_ERROR "DS_GAME_DIR (standalone dinput8.dll) and DS2_GAME_DIR (PSE tree) both set - pick one")
    endif()
    set(PSE_SRC "${PSE_REPO_ABS}/pse")
    set(DS2_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")   # for plugins/<name>/CMakeLists.txt

    # ds2_ship_target(<target> <description> <file name>): dev = PDB, DS_DIST = no PDB / timestamp / Rich header, both
    # with a version resource (DS1's ds_ship_target with DesertStorm2Fix's version). Applied to every shipped binary,
    # the DS2 plugins included (so a plugin adds no version resource of its own).
    function(ds2_ship_target t description filename)
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
        configure_file(${DS2_SOURCE_DIR}/src/pse/version_pse.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
        target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
    endfunction()

    # The PSE runtime, in its own output folders (the shim is also called dinput8.dll / dinput8.lib).
    file(GLOB DS2_PSE_CORE_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/loader/*.cpp)
    add_library(ds2_pse_core STATIC ${DS2_PSE_CORE_SOURCES})
    target_include_directories(ds2_pse_core PUBLIC ${PSE_SRC}/include PRIVATE ${PSE_SRC}/loader)
    target_compile_definitions(ds2_pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ds2_pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(ds2_pse_core PUBLIC advapi32 bcrypt)
    add_library(ds2_pse_hostlink STATIC ${PSE_SRC}/hostlink/hostlink.cpp ${PSE_SRC}/loader/compat.cpp
        ${PSE_SRC}/loader/x86len.cpp ${PSE_SRC}/loader/settings_migrate.cpp ${PSE_SRC}/loader/util.cpp)
    target_include_directories(ds2_pse_hostlink PUBLIC ${PSE_SRC}/include ${PSE_SRC}/loader ${PSE_SRC}/hostlink)
    target_compile_definitions(ds2_pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ds2_pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(ds2_pse_hostlink PUBLIC advapi32 bcrypt)

    add_library(ds2_pse_loader SHARED ${PSE_SRC}/loader_dll/loader_main.cpp)
    target_include_directories(ds2_pse_loader PRIVATE ${PSE_SRC}/loader)
    target_link_libraries(ds2_pse_loader PRIVATE ds2_pse_core)
    add_library(ds2_pse_shim SHARED ${PSE_SRC}/shim/shim.cpp ${PSE_SRC}/shim/proxy_dinput8.cpp ${PSE_SRC}/shim/dinput8.def)
    target_include_directories(ds2_pse_shim PRIVATE ${PSE_SRC}/include ${PSE_SRC}/loader)
    file(GLOB DS2_PSE_LAYER_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/games/ds2/*.cpp ${PSE_SRC}/games/ds2/*.h)
    add_library(ds2_pse_layer SHARED ${DS2_PSE_LAYER_SOURCES} ${PSE_SRC}/loader/ids.cpp)
    target_include_directories(ds2_pse_layer PRIVATE ${PSE_SRC}/games/ds2 ${PSE_SRC}/ids)   # generated/pse_ids_ds2.h
    target_link_libraries(ds2_pse_layer PRIVATE ds2_pse_hostlink)
    foreach(t ds2_pse_loader ds2_pse_shim ds2_pse_layer)
        target_link_options(${t} PRIVATE /SAFESEH:NO)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime_lib")
    endforeach()
    set_target_properties(ds2_pse_loader PROPERTIES OUTPUT_NAME pse_loader)
    set_target_properties(ds2_pse_shim PROPERTIES OUTPUT_NAME dinput8)
    set_target_properties(ds2_pse_layer PROPERTIES OUTPUT_NAME pse_ds2)
    ds2_ship_target(ds2_pse_loader "Pivotal Script Extender loader" pse_loader.dll)
    ds2_ship_target(ds2_pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
    ds2_ship_target(ds2_pse_layer "Pivotal Script Extender - Conflict: Desert Storm II game layer" pse_ds2.dll)

    # ds2fix.dll + ds2fix.pse: DesertStorm2Fix as the host plugin (src/pse/ds2fix.cpp). Same features as dinput8.dll;
    # patches through the PSE broker (DesertStormFix's broker-backed src/core/patch.cpp at the repo root), pse:: calls
    # through pse/hostlink. #ifdef DS2_PSE_HOST = what differs from the standalone build (settings, D3D9 hooks ...).
    # widescreen / hud / hudfix / frontend / fontsharp are the plugin ds2fix.widescreen (plugins/widescreen).
    add_library(ds2fix SHARED
        src/pse/ds2fix.cpp src/pse/ds2fix.pse
        src/pse/settings_pse.cpp src/core/log.cpp src/core/gamepad.cpp
        src/features/gameexe.cpp src/features/devbg.cpp
        src/features/graphics.cpp src/features/cinematics.cpp
        src/features/menus.cpp src/features/splitscreen.cpp src/features/coop.cpp
        src/features/mods.cpp src/features/modsmenu.cpp src/features/skins.cpp src/features/mods_levels.cpp
        src/features/mods_sound.cpp src/features/mods_tables.cpp src/features/weaponicons.cpp
        src/features/scripthost.cpp src/features/campaigns.cpp
        ${PSE_REPO_ABS}/games/ds1/src/core/patch.cpp)
    set_source_files_properties(src/pse/ds2fix.pse PROPERTIES HEADER_FILE_ONLY ON)
    # src/pse/shim first: its core/patch.h / core/pse_host.h / core/comhooks.h replace this repo's for the host build.
    target_include_directories(ds2fix PRIVATE src/pse/shim src)
    target_compile_definitions(ds2fix PRIVATE DS2_PSE_HOST DS_VERSION="${PROJECT_VERSION}"
        "DS2_PSE_PATCH_H=\"${PSE_REPO_ABS}/games/ds1/src/core/patch.h\"" "DS2_PSE_COMHOOKS_H=\"${PSE_REPO_ABS}/games/ds1/src/core/comhooks.h\"")
    target_link_libraries(ds2fix PRIVATE ds2_pse_hostlink ds2_lua winmm dxguid dinput8.lib ole32 d3d9 advapi32 bcrypt windowscodecs)
    target_link_options(ds2fix PRIVATE /SAFESEH:NO)
    set_target_properties(ds2fix PROPERTIES OUTPUT_NAME ds2fix PREFIX "")
    ds2_ship_target(ds2fix "DesertStorm2Fix - Conflict: Desert Storm II enhancement patch (PSE host plugin)" ds2fix.dll)
    add_custom_command(TARGET ds2fix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/pse/ds2fix.pse" "$<TARGET_FILE_DIR:ds2fix>"
        COMMENT "ds2fix.pse next to ds2fix.dll")

    # DesertStorm2Fix-Patcher.exe: turns the 4 GB (Large Address Aware) flag of CDS II.exe on / off - DS1's patcher
    # (repo root src/patcher) built for DS2 (PATCHER_GAME_DS2: CDS II.exe 1.0, never the SecuROM exe). Shipped next to
    # the exe (tree, package, DS2_GAME_DIR); the plugin ds2fix.memory reports the 2 GB / 4 GB state.
    set(PATCHER_MANIFEST "${PSE_REPO_ABS}/games/ds1/src/patcher/patcher.manifest")
    set(PATCHER_DESCRIPTION "DesertStorm2Fix Patcher (4 GB memory for Conflict: Desert Storm II)")
    set(PATCHER_NAME DesertStorm2Fix-Patcher)
    set(PATCHER_PRODUCT DesertStorm2Fix)
    set(DS_VERSION_FULL "${PROJECT_VERSION}")
    configure_file(${PSE_REPO_ABS}/games/ds1/src/patcher/patcher.rc.in ${CMAKE_BINARY_DIR}/ds2_patcher.rc @ONLY)
    add_executable(ds2_patcher WIN32 ${PSE_REPO_ABS}/games/ds1/src/patcher/patcher.cpp ${CMAKE_BINARY_DIR}/ds2_patcher.rc)
    set_target_properties(ds2_patcher PROPERTIES OUTPUT_NAME DesertStorm2Fix-Patcher
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/patcher)
    set_property(SOURCE ${CMAKE_BINARY_DIR}/ds2_patcher.rc APPEND PROPERTY OBJECT_DEPENDS "${PATCHER_MANIFEST}")
    target_compile_definitions(ds2_patcher PRIVATE PATCHER_GAME_DS2 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ds2_patcher PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(ds2_patcher PRIVATE bcrypt comctl32 shell32 ole32 advapi32)
    target_link_options(ds2_patcher PRIVATE /MANIFEST:NO $<$<BOOL:${DS_DIST}>:/DEBUG:NONE /Brepro /EMITTOOLVERSIONINFO:NO>)
    if(DS_DIST)
        target_compile_options(ds2_patcher PRIVATE /Brepro)
    endif()

    # DS2 plugins: every plugins/<name>/CMakeLists.txt (picked up by this glob at configure time), each building its
    # DLL with pse_add_plugin() (pse/cmake/PSEPlugin.cmake: SDK header only, static CRT, manifest next to the DLL).
    # Shipped into PSE\Plugins (tree, package, DS2_GAME_DIR) and given ds2_ship_target here. Available to them:
    # PSE_REPO_ABS, PSE_SRC, DS2_SOURCE_DIR, DS_DIST, ds2_pse_hostlink. docs/sdk/reference/game-ds2.md "DS2 plugins".
    include(${PSE_SRC}/cmake/PSEPlugin.cmake)
    set(DS2_PLUGINS "")
    file(GLOB DS2_PLUGIN_LISTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*/CMakeLists.txt)
    set(DS2_SKIP_PLUGINS "" CACHE STRING "plugins/<name> folders left out of this build (;-list, e.g. one being written)")
    foreach(list_file ${DS2_PLUGIN_LISTS})
        get_filename_component(dir "${list_file}" DIRECTORY)
        get_filename_component(name "${dir}" NAME)
        if(name IN_LIST DS2_SKIP_PLUGINS)
            message(STATUS "DS2 plugin ${name} skipped (DS2_SKIP_PLUGINS)")
            continue()
        endif()
        add_subdirectory("${dir}" "${CMAKE_CURRENT_BINARY_DIR}/plugins/${name}")
        get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t ${targets})
            get_target_property(manifest ${t} PSE_MANIFEST)
            if(manifest)
                get_filename_component(stem "${manifest}" NAME_WLE)
                ds2_ship_target(${t} "DesertStorm2Fix plugin ${stem}" ${stem}.dll)
                list(APPEND DS2_PLUGINS ${t})
            endif()
        endforeach()
    endforeach()
    list(LENGTH DS2_PLUGINS DS2_PLUGIN_COUNT)
    message(STATUS "DS2 plugins (games/ds2/plugins): ${DS2_PLUGIN_COUNT} ${DS2_PLUGINS}")

    # The install tree (assemble.cmake: "<src>|<dest>@..."; "?" = optional - the PDBs).
    set(DS2_TREE "$<TARGET_FILE:ds2_pse_shim>|dinput8.dll@$<TARGET_FILE:ds2_pse_loader>|PSE/pse_loader.dll")
    string(APPEND DS2_TREE "@$<TARGET_FILE:ds2_pse_layer>|PSE/Games/pse_ds2.dll")
    string(APPEND DS2_TREE "@$<TARGET_FILE:ds2fix>|PSE/Plugins/ds2fix.dll")
    string(APPEND DS2_TREE "@${CMAKE_CURRENT_SOURCE_DIR}/src/pse/ds2fix.pse|PSE/Plugins/ds2fix.pse")
    string(APPEND DS2_TREE "@$<TARGET_FILE:ds2_patcher>|DesertStorm2Fix-Patcher.exe")
    set(DS2_TREE_PDBS "?$<TARGET_PDB_FILE:ds2_pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:ds2_pse_loader>|PSE/pse_loader.pdb")
    string(APPEND DS2_TREE_PDBS "@?$<TARGET_PDB_FILE:ds2_pse_layer>|PSE/Games/pse_ds2.pdb")
    string(APPEND DS2_TREE_PDBS "@?$<TARGET_PDB_FILE:ds2fix>|PSE/Plugins/ds2fix.pdb")
    set(DS2_TREE_DEPENDS ds2_pse_shim ds2_pse_loader ds2_pse_layer ds2fix ds2_patcher
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pse/ds2fix.pse)
    foreach(t ${DS2_PLUGINS})
        get_target_property(manifest ${t} PSE_MANIFEST)
        get_filename_component(stem "${manifest}" NAME_WLE)
        string(APPEND DS2_TREE "@$<TARGET_FILE:${t}>|PSE/Plugins/${stem}.dll@${manifest}|PSE/Plugins/${stem}.pse")
        string(APPEND DS2_TREE_PDBS "@?$<TARGET_PDB_FILE:${t}>|PSE/Plugins/${stem}.pdb")
        list(APPEND DS2_TREE_DEPENDS ${t} ${manifest})
    endforeach()
    set(DS2_ASSEMBLE ${PSE_SRC}/cmake/assemble.cmake)
    if(DS_DIST)
        set(DS2_TREE_FILES "${DS2_TREE}")
    else()
        set(DS2_TREE_FILES "${DS2_TREE}@${DS2_TREE_PDBS}")
    endif()
    set(DS2_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> "-DFILES=${DS2_TREE_FILES}"
        -DCLEAN=1 -P ${DS2_ASSEMBLE})
    if(DS_DIST)
        # The release package: the tree (no PDBs) + the readme, zipped.
        set(DS2_PACKAGE ${CMAKE_BINARY_DIR}/package)
        set(DS2_ZIP ${CMAKE_BINARY_DIR}/DesertStorm2Fix-${PROJECT_VERSION}.zip)
        list(APPEND DS2_TREE_COMMANDS
            COMMAND ${CMAKE_COMMAND} -DDEST=${DS2_PACKAGE} -DCLEAN=1
                "-DFILES=${DS2_TREE}@${CMAKE_CURRENT_SOURCE_DIR}/dist/DesertStorm2Fix-README.txt|DesertStorm2Fix-README.txt"
                -P ${DS2_ASSEMBLE}
            COMMAND ${CMAKE_COMMAND} -E rm -f ${DS2_ZIP}
            COMMAND ${CMAKE_COMMAND} -E chdir ${DS2_PACKAGE} ${CMAKE_COMMAND} -E tar cf ${DS2_ZIP} --format=zip .)
        list(APPEND DS2_TREE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dist/DesertStorm2Fix-README.txt)
    endif()
    if(DS2_GAME_DIR)
        # Into the game: changed files only (the standalone DesertStorm2Fix's dinput8.dll is replaced by the shim).
        list(APPEND DS2_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${DS2_GAME_DIR} "-DFILES=${DS2_TREE_FILES}"
            -P ${DS2_ASSEMBLE})
    endif()
    set(DS2_TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
    add_custom_command(OUTPUT ${DS2_TREE_STAMP}
        ${DS2_TREE_COMMANDS}
        COMMAND ${CMAKE_COMMAND} -E touch ${DS2_TREE_STAMP}
        DEPENDS ${DS2_TREE_DEPENDS} ${DS2_ASSEMBLE}
        COMMENT "DesertStorm2Fix PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>"
        VERBATIM)
    add_custom_target(ds2_pse_tree ALL DEPENDS ${DS2_TREE_STAMP})
endif()
