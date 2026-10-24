cmake_minimum_required(VERSION 3.21)
project(GreatEscapeFix VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GreatEscape.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# GreatEscapeFix on the Pivotal Script Extender (PSE) - the only build (no standalone dinput8.dll). The install tree
# (docs/sdk/layout.md, docs/sdk/reference/game-tge.md):
#   dinput8.dll                     the PSE shim (game-neutral, pse/shim)
#   PSE\pse_loader.dll              the loader (pse/loader + pse/loader_dll)
#   PSE\Games\pse_tge.dll           The Great Escape game layer (pse/games/tge)
#   PSE\Plugins\tgefix.dll + .pse   GreatEscapeFix as the host plugin (src/tgefix.cpp): widescreen, Mods folder
#   PSE\Plugins\<name>.dll + .pse   every TGE plugin in plugins/<name>/ (none yet)
# assembled in ${CMAKE_BINARY_DIR}/tree/<config>/ by every build (pse/cmake/assemble.cmake), copied into TGE_GAME_DIR
# when set (changed files only), and with DS_DIST packaged (no PDBs, + the readme) into package/ +
# GreatEscapeFix-<ver>.zip. From games/tge: cmake --preset x86 && cmake --build --preset release; from the repo root:
# cmake --preset tge && cmake --build --preset tge (release package: tge-dist).
set(TGE_GAME_DIR "" CACHE PATH "The Great Escape install folder: the PSE tree is copied there")
option(DS_DIST "Build the release package (no diagnostics)" OFF)
set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, games/ds1/src/core/patch.cpp)")
get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
    message(FATAL_ERROR "PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
endif()
set(PSE_SRC "${PSE_REPO_ABS}/pse")
set(TGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")   # for plugins/<name>/CMakeLists.txt

# tge_ship_target(<target> <description> <file name>): dev = PDB, DS_DIST = no PDB / timestamp / Rich header, both with
# a version resource (DS1's ds_ship_target with GreatEscapeFix's version).
function(tge_ship_target t description filename)
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
    configure_file(${TGE_SOURCE_DIR}/src/version.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
    target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
endfunction()

# ---- the PSE runtime, in its own output folders (the shim is also called dinput8.dll / dinput8.lib) ----
file(GLOB TGE_PSE_CORE_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/loader/*.cpp)
add_library(tge_pse_core STATIC ${TGE_PSE_CORE_SOURCES})
target_include_directories(tge_pse_core PUBLIC ${PSE_SRC}/include PRIVATE ${PSE_SRC}/loader)
target_compile_definitions(tge_pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(tge_pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(tge_pse_core PUBLIC advapi32 bcrypt)
add_library(tge_pse_hostlink STATIC ${PSE_SRC}/hostlink/hostlink.cpp ${PSE_SRC}/loader/compat.cpp
    ${PSE_SRC}/loader/x86len.cpp ${PSE_SRC}/loader/settings_migrate.cpp ${PSE_SRC}/loader/util.cpp)
target_include_directories(tge_pse_hostlink PUBLIC ${PSE_SRC}/include ${PSE_SRC}/loader ${PSE_SRC}/hostlink)
target_compile_definitions(tge_pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(tge_pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(tge_pse_hostlink PUBLIC advapi32 bcrypt)

add_library(tge_pse_loader SHARED ${PSE_SRC}/loader_dll/loader_main.cpp)
target_include_directories(tge_pse_loader PRIVATE ${PSE_SRC}/loader)
target_link_libraries(tge_pse_loader PRIVATE tge_pse_core)
add_library(tge_pse_shim SHARED ${PSE_SRC}/shim/shim.cpp ${PSE_SRC}/shim/proxy_dinput8.cpp ${PSE_SRC}/shim/dinput8.def)
target_include_directories(tge_pse_shim PRIVATE ${PSE_SRC}/include ${PSE_SRC}/loader)
file(GLOB TGE_PSE_LAYER_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/games/tge/*.cpp ${PSE_SRC}/games/tge/*.h)
add_library(tge_pse_layer SHARED ${TGE_PSE_LAYER_SOURCES} ${PSE_SRC}/loader/ids.cpp)
target_include_directories(tge_pse_layer PRIVATE ${PSE_SRC}/games/tge ${PSE_SRC}/ids)   # generated/pse_ids_tge.h
target_link_libraries(tge_pse_layer PRIVATE tge_pse_hostlink)
foreach(t tge_pse_loader tge_pse_shim tge_pse_layer)
    target_link_options(${t} PRIVATE /SAFESEH:NO)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime_lib")
endforeach()
set_target_properties(tge_pse_loader PROPERTIES OUTPUT_NAME pse_loader)
set_target_properties(tge_pse_shim PROPERTIES OUTPUT_NAME dinput8)
set_target_properties(tge_pse_layer PROPERTIES OUTPUT_NAME pse_tge)
tge_ship_target(tge_pse_loader "Pivotal Script Extender loader" pse_loader.dll)
tge_ship_target(tge_pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
tge_ship_target(tge_pse_layer "Pivotal Script Extender - The Great Escape game layer" pse_tge.dll)

# ---- tgefix.dll + tgefix.pse: GreatEscapeFix as the host plugin (src/tgefix.cpp) ----
# Patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp (src/core/patch.h forwards to its header), so
# every byte group / hook has an owner: the feature's sub-plugin "tgefix.<feature>". pse:: calls through pse/hostlink.
add_library(tgefix SHARED
    src/tgefix.cpp src/tgefix.pse
    src/core/log.cpp
    src/features/widescreen.cpp src/features/mods.cpp src/features/display.cpp src/features/guiscale.cpp src/features/framecap.cpp src/features/devbg.cpp
    ${PSE_REPO_ABS}/games/ds1/src/core/patch.cpp)
set_source_files_properties(src/tgefix.pse PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(tgefix PRIVATE src)
target_compile_definitions(tgefix PRIVATE DS_VERSION="${PROJECT_VERSION}"
    "TGE_PSE_PATCH_H=\"${PSE_REPO_ABS}/games/ds1/src/core/patch.h\"")
target_link_libraries(tgefix PRIVATE tge_pse_hostlink advapi32 bcrypt user32 ole32)
target_link_options(tgefix PRIVATE /SAFESEH:NO)
set_target_properties(tgefix PROPERTIES OUTPUT_NAME tgefix PREFIX "")
tge_ship_target(tgefix "GreatEscapeFix - The Great Escape enhancement patch (PSE host plugin)" tgefix.dll)

# ---- TGE plugins: every plugins/<name>/CMakeLists.txt, each building its DLL with pse_add_plugin() ----
include(${PSE_SRC}/cmake/PSEPlugin.cmake)
set(TGE_PLUGINS "")
file(GLOB TGE_PLUGIN_LISTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*/CMakeLists.txt)
foreach(list_file ${TGE_PLUGIN_LISTS})
    get_filename_component(dir "${list_file}" DIRECTORY)
    get_filename_component(name "${dir}" NAME)
    add_subdirectory("${dir}" "${CMAKE_CURRENT_BINARY_DIR}/plugins/${name}")
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t ${targets})
        get_target_property(manifest ${t} PSE_MANIFEST)
        if(manifest)
            get_filename_component(stem "${manifest}" NAME_WLE)
            tge_ship_target(${t} "GreatEscapeFix plugin ${stem}" ${stem}.dll)
            list(APPEND TGE_PLUGINS ${t})
        endif()
    endforeach()
endforeach()

# ---- the install tree (assemble.cmake: "<src>|<dest>@..."; "?" = optional - the PDBs) ----
set(TGE_TREE "$<TARGET_FILE:tge_pse_shim>|dinput8.dll@$<TARGET_FILE:tge_pse_loader>|PSE/pse_loader.dll")
string(APPEND TGE_TREE "@$<TARGET_FILE:tge_pse_layer>|PSE/Games/pse_tge.dll")
string(APPEND TGE_TREE "@$<TARGET_FILE:tgefix>|PSE/Plugins/tgefix.dll")
string(APPEND TGE_TREE "@${CMAKE_CURRENT_SOURCE_DIR}/src/tgefix.pse|PSE/Plugins/tgefix.pse")
set(TGE_TREE_PDBS "?$<TARGET_PDB_FILE:tge_pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:tge_pse_loader>|PSE/pse_loader.pdb")
string(APPEND TGE_TREE_PDBS "@?$<TARGET_PDB_FILE:tge_pse_layer>|PSE/Games/pse_tge.pdb")
string(APPEND TGE_TREE_PDBS "@?$<TARGET_PDB_FILE:tgefix>|PSE/Plugins/tgefix.pdb")
set(TGE_TREE_DEPENDS tge_pse_shim tge_pse_loader tge_pse_layer tgefix ${CMAKE_CURRENT_SOURCE_DIR}/src/tgefix.pse)
foreach(t ${TGE_PLUGINS})
    get_target_property(manifest ${t} PSE_MANIFEST)
    get_filename_component(stem "${manifest}" NAME_WLE)
    string(APPEND TGE_TREE "@$<TARGET_FILE:${t}>|PSE/Plugins/${stem}.dll@${manifest}|PSE/Plugins/${stem}.pse")
    string(APPEND TGE_TREE_PDBS "@?$<TARGET_PDB_FILE:${t}>|PSE/Plugins/${stem}.pdb")
    list(APPEND TGE_TREE_DEPENDS ${t} ${manifest})
endforeach()
set(TGE_ASSEMBLE ${PSE_SRC}/cmake/assemble.cmake)
if(DS_DIST)
    set(TGE_TREE_FILES "${TGE_TREE}")
else()
    set(TGE_TREE_FILES "${TGE_TREE}@${TGE_TREE_PDBS}")
endif()
set(TGE_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> "-DFILES=${TGE_TREE_FILES}"
    -DCLEAN=1 -P ${TGE_ASSEMBLE})
if(DS_DIST)
    set(TGE_PACKAGE ${CMAKE_BINARY_DIR}/package)
    set(TGE_ZIP ${CMAKE_BINARY_DIR}/GreatEscapeFix-${PROJECT_VERSION}.zip)
    list(APPEND TGE_TREE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -DDEST=${TGE_PACKAGE} -DCLEAN=1
            "-DFILES=${TGE_TREE}@${CMAKE_CURRENT_SOURCE_DIR}/dist/GreatEscapeFix-README.txt|GreatEscapeFix-README.txt"
            -P ${TGE_ASSEMBLE}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${TGE_ZIP}
        COMMAND ${CMAKE_COMMAND} -E chdir ${TGE_PACKAGE} ${CMAKE_COMMAND} -E tar cf ${TGE_ZIP} --format=zip .)
    list(APPEND TGE_TREE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dist/GreatEscapeFix-README.txt)
endif()
if(TGE_GAME_DIR)
    list(APPEND TGE_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${TGE_GAME_DIR} "-DFILES=${TGE_TREE_FILES}"
        -P ${TGE_ASSEMBLE})
endif()
set(TGE_TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
add_custom_command(OUTPUT ${TGE_TREE_STAMP}
    ${TGE_TREE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E touch ${TGE_TREE_STAMP}
    DEPENDS ${TGE_TREE_DEPENDS} ${TGE_ASSEMBLE}
    COMMENT "GreatEscapeFix PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>"
    VERBATIM)
add_custom_target(tge_pse_tree ALL DEPENDS ${TGE_TREE_STAMP})
