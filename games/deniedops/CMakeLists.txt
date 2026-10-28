cmake_minimum_required(VERSION 3.21)
project(DeniedOpsFix VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ConflictDeniedOps.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# DeniedOpsFix on the Pivotal Script Extender (PSE) - PSE-only (no standalone dinput8.dll), same shape as GreatEscapeFix
# (games/tge). Install tree (docs/sdk/layout.md, docs/sdk/reference/game-deniedops.md):
#   dinput8.dll                           the PSE shim (game-neutral, pse/shim)
#   PSE\pse_loader.dll                    the loader (pse/loader + pse/loader_dll)
#   PSE\Games\pse_deniedops.dll           Conflict: Denied Ops game layer (pse/games/deniedops)
#   PSE\Plugins\deniedopsfix.dll + .pse   DeniedOpsFix as the host plugin (src/pse/deniedopsfix.cpp): widescreen
#                                          modes + Mods folder
# assembled in ${CMAKE_BINARY_DIR}/tree/<config>/ by every build (pse/cmake/assemble.cmake), copied into
# DENIEDOPS_GAME_DIR when set (changed files only). From games/deniedops: cmake --preset x86 && cmake --build --preset
# release; from the repo root: cmake --preset deniedops-pse && cmake --build --preset deniedops-pse.
set(DENIEDOPS_GAME_DIR "" CACHE PATH "Conflict: Denied Ops install folder: the PSE tree is copied there")
option(DS_DIST "Build the release package (no diagnostics)" OFF)
set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, games/ds1/src/core/patch.cpp)")
get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
    message(FATAL_ERROR "PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
endif()
set(PSE_SRC "${PSE_REPO_ABS}/pse")
set(DENIEDOPS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# deniedops_ship_target(<target> <description> <file name>): dev = PDB, DS_DIST = no PDB / timestamp / Rich header.
function(deniedops_ship_target t description filename)
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
    configure_file(${DENIEDOPS_SOURCE_DIR}/src/version.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
    target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
endfunction()

# ---- the PSE runtime, in its own output folders ----
file(GLOB DENIEDOPS_PSE_CORE_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/loader/*.cpp)
add_library(deniedops_pse_core STATIC ${DENIEDOPS_PSE_CORE_SOURCES})
target_include_directories(deniedops_pse_core PUBLIC ${PSE_SRC}/include PRIVATE ${PSE_SRC}/loader)
target_compile_definitions(deniedops_pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(deniedops_pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(deniedops_pse_core PUBLIC advapi32 bcrypt)
add_library(deniedops_pse_hostlink STATIC ${PSE_SRC}/hostlink/hostlink.cpp ${PSE_SRC}/loader/compat.cpp
    ${PSE_SRC}/loader/x86len.cpp ${PSE_SRC}/loader/settings_migrate.cpp ${PSE_SRC}/loader/util.cpp)
target_include_directories(deniedops_pse_hostlink PUBLIC ${PSE_SRC}/include ${PSE_SRC}/loader ${PSE_SRC}/hostlink)
target_compile_definitions(deniedops_pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(deniedops_pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(deniedops_pse_hostlink PUBLIC advapi32 bcrypt)

add_library(deniedops_pse_loader SHARED ${PSE_SRC}/loader_dll/loader_main.cpp)
target_include_directories(deniedops_pse_loader PRIVATE ${PSE_SRC}/loader)
target_link_libraries(deniedops_pse_loader PRIVATE deniedops_pse_core)
add_library(deniedops_pse_shim SHARED ${PSE_SRC}/shim/shim.cpp ${PSE_SRC}/shim/proxy_dinput8.cpp ${PSE_SRC}/shim/dinput8.def)
target_include_directories(deniedops_pse_shim PRIVATE ${PSE_SRC}/include ${PSE_SRC}/loader)
file(GLOB DENIEDOPS_PSE_LAYER_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/games/deniedops/*.cpp ${PSE_SRC}/games/deniedops/*.h)
add_library(deniedops_pse_layer SHARED ${DENIEDOPS_PSE_LAYER_SOURCES} ${PSE_SRC}/loader/ids.cpp)
target_include_directories(deniedops_pse_layer PRIVATE ${PSE_SRC}/games/deniedops ${PSE_SRC}/ids)   # generated/pse_ids_deniedops.h
target_link_libraries(deniedops_pse_layer PRIVATE deniedops_pse_hostlink)
foreach(t deniedops_pse_loader deniedops_pse_shim deniedops_pse_layer)
    target_link_options(${t} PRIVATE /SAFESEH:NO)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime_lib")
endforeach()
set_target_properties(deniedops_pse_loader PROPERTIES OUTPUT_NAME pse_loader)
set_target_properties(deniedops_pse_shim PROPERTIES OUTPUT_NAME dinput8)
set_target_properties(deniedops_pse_layer PROPERTIES OUTPUT_NAME pse_deniedops)
deniedops_ship_target(deniedops_pse_loader "Pivotal Script Extender loader" pse_loader.dll)
deniedops_ship_target(deniedops_pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
deniedops_ship_target(deniedops_pse_layer "Pivotal Script Extender - Conflict: Denied Ops game layer" pse_deniedops.dll)

# ---- deniedopsfix.dll + .pse: DeniedOpsFix as the host plugin (src/pse/deniedopsfix.cpp) ----
# Patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp, so every byte group / hook has an owner:
# the feature's sub-plugin "deniedopsfix.<feature>". pse:: calls through pse/hostlink.
add_library(deniedopsfix SHARED
    src/pse/deniedopsfix.cpp src/pse/deniedopsfix.pse
    src/core/log.cpp
    src/features/unpack.cpp src/features/widescreen.cpp src/features/mods.cpp src/features/modsmenu.cpp src/features/devinput.cpp
    ${PSE_REPO_ABS}/games/ds1/src/core/patch.cpp)
set_source_files_properties(src/pse/deniedopsfix.pse PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(deniedopsfix PRIVATE src)
target_compile_definitions(deniedopsfix PRIVATE DS_VERSION="${PROJECT_VERSION}"
    "DENIEDOPS_PSE_PATCH_H=\"${PSE_REPO_ABS}/games/ds1/src/core/patch.h\"")
target_link_libraries(deniedopsfix PRIVATE deniedops_pse_hostlink advapi32 bcrypt user32 ole32)
target_link_options(deniedopsfix PRIVATE /SAFESEH:NO)
set_target_properties(deniedopsfix PROPERTIES OUTPUT_NAME deniedopsfix PREFIX "")
deniedops_ship_target(deniedopsfix "DeniedOpsFix - Conflict: Denied Ops enhancement patch (PSE host plugin)" deniedopsfix.dll)

# ---- the install tree (assemble.cmake: "<src>|<dest>@..."; "?" = optional - the PDBs) ----
set(DENIEDOPS_TREE "$<TARGET_FILE:deniedops_pse_shim>|dinput8.dll@$<TARGET_FILE:deniedops_pse_loader>|PSE/pse_loader.dll")
string(APPEND DENIEDOPS_TREE "@$<TARGET_FILE:deniedops_pse_layer>|PSE/Games/pse_deniedops.dll")
string(APPEND DENIEDOPS_TREE "@$<TARGET_FILE:deniedopsfix>|PSE/Plugins/deniedopsfix.dll")
string(APPEND DENIEDOPS_TREE "@${CMAKE_CURRENT_SOURCE_DIR}/src/pse/deniedopsfix.pse|PSE/Plugins/deniedopsfix.pse")
set(DENIEDOPS_TREE_PDBS "?$<TARGET_PDB_FILE:deniedops_pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:deniedops_pse_loader>|PSE/pse_loader.pdb")
string(APPEND DENIEDOPS_TREE_PDBS "@?$<TARGET_PDB_FILE:deniedops_pse_layer>|PSE/Games/pse_deniedops.pdb")
string(APPEND DENIEDOPS_TREE_PDBS "@?$<TARGET_PDB_FILE:deniedopsfix>|PSE/Plugins/deniedopsfix.pdb")
set(DENIEDOPS_TREE_DEPENDS deniedops_pse_shim deniedops_pse_loader deniedops_pse_layer deniedopsfix
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pse/deniedopsfix.pse)
set(DENIEDOPS_ASSEMBLE ${PSE_SRC}/cmake/assemble.cmake)
if(DS_DIST)
    set(DENIEDOPS_TREE_FILES "${DENIEDOPS_TREE}")
else()
    set(DENIEDOPS_TREE_FILES "${DENIEDOPS_TREE}@${DENIEDOPS_TREE_PDBS}")
endif()
set(DENIEDOPS_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG>
    "-DFILES=${DENIEDOPS_TREE_FILES}" -DCLEAN=1 -P ${DENIEDOPS_ASSEMBLE})
if(DS_DIST)
    set(DENIEDOPS_PACKAGE ${CMAKE_BINARY_DIR}/package)
    set(DENIEDOPS_ZIP ${CMAKE_BINARY_DIR}/DeniedOpsFix-${PROJECT_VERSION}.zip)
    list(APPEND DENIEDOPS_TREE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -DDEST=${DENIEDOPS_PACKAGE} -DCLEAN=1 "-DFILES=${DENIEDOPS_TREE}"
            -P ${DENIEDOPS_ASSEMBLE}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${DENIEDOPS_ZIP}
        COMMAND ${CMAKE_COMMAND} -E chdir ${DENIEDOPS_PACKAGE} ${CMAKE_COMMAND} -E tar cf ${DENIEDOPS_ZIP} --format=zip .)
endif()
if(DENIEDOPS_GAME_DIR)
    list(APPEND DENIEDOPS_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${DENIEDOPS_GAME_DIR}
        "-DFILES=${DENIEDOPS_TREE_FILES}" -P ${DENIEDOPS_ASSEMBLE})
endif()
set(DENIEDOPS_TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
add_custom_command(OUTPUT ${DENIEDOPS_TREE_STAMP}
    ${DENIEDOPS_TREE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E touch ${DENIEDOPS_TREE_STAMP}
    DEPENDS ${DENIEDOPS_TREE_DEPENDS} ${DENIEDOPS_ASSEMBLE}
    COMMENT "DeniedOpsFix PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>"
    VERBATIM)
add_custom_target(deniedops_pse_tree ALL DEPENDS ${DENIEDOPS_TREE_STAMP})
