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

# ============================================================================================================
# GlobalStormFix on the Pivotal Script Extender (opt-in, CGS_PSE_PLUGIN=ON): shim -> PSE\pse_loader.dll ->
# PSE\Games\pse_cgs.dll (pse/games/cgs) -> PSE\Plugins\cgsfix.dll + .pse (this project's src/pse/cgsfix.cpp: widescreen
# + Mods folder only, routed through DesertStormFix's broker-backed games/ds1/src/core/patch.cpp - docs/sdk/
# reference/game-cgs.md). The standalone dinput8.dll build above is unaffected and keeps building either way (same
# precedent as DS2: both builds exist side by side). From games/cgs: cmake --preset x86 -DCGS_PSE_PLUGIN=ON; from the
# repo root: cmake --preset cgs-pse && cmake --build --preset cgs-pse (release package: cgs-pse-dist).
option(CGS_PSE_PLUGIN "Also build GlobalStormFix as a PSE host plugin (cgsfix.dll + shim/loader/layer, tree)" OFF)
if(CGS_PSE_PLUGIN)
    set(CGS_GAME_DIR "" CACHE PATH "Conflict: Global Storm install folder: the PSE tree is copied there")
    set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, games/ds1/src/core/patch.cpp)")
    get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
    if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
        message(FATAL_ERROR "PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
    endif()
    set(PSE_SRC "${PSE_REPO_ABS}/pse")
    set(CGS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    function(cgs_ship_target t description filename)
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
        configure_file(${CGS_SOURCE_DIR}/src/version.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
        target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
    endfunction()

    file(GLOB CGS_PSE_CORE_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/loader/*.cpp)
    add_library(cgs_pse_core STATIC ${CGS_PSE_CORE_SOURCES})
    target_include_directories(cgs_pse_core PUBLIC ${PSE_SRC}/include PRIVATE ${PSE_SRC}/loader)
    target_compile_definitions(cgs_pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cgs_pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(cgs_pse_core PUBLIC advapi32 bcrypt)
    add_library(cgs_pse_hostlink STATIC ${PSE_SRC}/hostlink/hostlink.cpp ${PSE_SRC}/loader/compat.cpp
        ${PSE_SRC}/loader/x86len.cpp ${PSE_SRC}/loader/settings_migrate.cpp ${PSE_SRC}/loader/util.cpp)
    target_include_directories(cgs_pse_hostlink PUBLIC ${PSE_SRC}/include ${PSE_SRC}/loader ${PSE_SRC}/hostlink)
    target_compile_definitions(cgs_pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cgs_pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(cgs_pse_hostlink PUBLIC advapi32 bcrypt)

    add_library(cgs_pse_loader SHARED ${PSE_SRC}/loader_dll/loader_main.cpp)
    target_include_directories(cgs_pse_loader PRIVATE ${PSE_SRC}/loader)
    target_link_libraries(cgs_pse_loader PRIVATE cgs_pse_core)
    add_library(cgs_pse_shim SHARED ${PSE_SRC}/shim/shim.cpp ${PSE_SRC}/shim/proxy_dinput8.cpp ${PSE_SRC}/shim/dinput8.def)
    target_include_directories(cgs_pse_shim PRIVATE ${PSE_SRC}/include ${PSE_SRC}/loader)
    file(GLOB CGS_PSE_LAYER_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/games/cgs/*.cpp ${PSE_SRC}/games/cgs/*.h)
    add_library(cgs_pse_layer SHARED ${CGS_PSE_LAYER_SOURCES} ${PSE_SRC}/loader/ids.cpp)
    target_include_directories(cgs_pse_layer PRIVATE ${PSE_SRC}/games/cgs ${PSE_SRC}/ids)
    target_link_libraries(cgs_pse_layer PRIVATE cgs_pse_hostlink)
    foreach(t cgs_pse_loader cgs_pse_shim cgs_pse_layer)
        target_link_options(${t} PRIVATE /SAFESEH:NO)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime_lib")
    endforeach()
    set_target_properties(cgs_pse_loader PROPERTIES OUTPUT_NAME pse_loader)
    set_target_properties(cgs_pse_shim PROPERTIES OUTPUT_NAME dinput8)
    set_target_properties(cgs_pse_layer PROPERTIES OUTPUT_NAME pse_cgs)
    cgs_ship_target(cgs_pse_loader "Pivotal Script Extender loader" pse_loader.dll)
    cgs_ship_target(cgs_pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
    cgs_ship_target(cgs_pse_layer "Pivotal Script Extender - Global Storm game layer" pse_cgs.dll)

    # ---- cgsfix.dll + cgsfix.pse ----
    add_library(cgsfix SHARED
        src/pse/cgsfix.cpp src/pse/cgsfix.pse
        src/core/log.cpp
        src/pse/features/widescreen.cpp src/pse/features/mods.cpp
        ${PSE_REPO_ABS}/games/ds1/src/core/patch.cpp)
    set_source_files_properties(src/pse/cgsfix.pse PROPERTIES HEADER_FILE_ONLY ON)
    # src/pse first: its core/patch.h + features/features.h (own, smaller Settings) shadow the standalone's src/ ones
    # for this target only - the standalone dinput8 target above is untouched (it only sees src/).
    target_include_directories(cgsfix PRIVATE src/pse src)
    target_compile_definitions(cgsfix PRIVATE DS_VERSION="${PROJECT_VERSION}"
        "CGS_PSE_PATCH_H=\"${PSE_REPO_ABS}/games/ds1/src/core/patch.h\"")
    target_link_libraries(cgsfix PRIVATE cgs_pse_hostlink advapi32 bcrypt user32 ole32)
    target_link_options(cgsfix PRIVATE /SAFESEH:NO)
    set_target_properties(cgsfix PROPERTIES OUTPUT_NAME cgsfix PREFIX "")
    cgs_ship_target(cgsfix "GlobalStormFix - Conflict: Global Storm enhancement patch (PSE host plugin)" cgsfix.dll)

    # ---- the install tree ----
    set(CGS_TREE "$<TARGET_FILE:cgs_pse_shim>|dinput8.dll@$<TARGET_FILE:cgs_pse_loader>|PSE/pse_loader.dll")
    string(APPEND CGS_TREE "@$<TARGET_FILE:cgs_pse_layer>|PSE/Games/pse_cgs.dll")
    string(APPEND CGS_TREE "@$<TARGET_FILE:cgsfix>|PSE/Plugins/cgsfix.dll")
    string(APPEND CGS_TREE "@${CMAKE_CURRENT_SOURCE_DIR}/src/pse/cgsfix.pse|PSE/Plugins/cgsfix.pse")
    set(CGS_TREE_PDBS "?$<TARGET_PDB_FILE:cgs_pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:cgs_pse_loader>|PSE/pse_loader.pdb")
    string(APPEND CGS_TREE_PDBS "@?$<TARGET_PDB_FILE:cgs_pse_layer>|PSE/Games/pse_cgs.pdb")
    string(APPEND CGS_TREE_PDBS "@?$<TARGET_PDB_FILE:cgsfix>|PSE/Plugins/cgsfix.pdb")
    set(CGS_TREE_DEPENDS cgs_pse_shim cgs_pse_loader cgs_pse_layer cgsfix ${CMAKE_CURRENT_SOURCE_DIR}/src/pse/cgsfix.pse)
    set(CGS_ASSEMBLE ${PSE_SRC}/cmake/assemble.cmake)
    if(DS_DIST)
        set(CGS_TREE_FILES "${CGS_TREE}")
    else()
        set(CGS_TREE_FILES "${CGS_TREE}@${CGS_TREE_PDBS}")
    endif()
    set(CGS_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> "-DFILES=${CGS_TREE_FILES}"
        -DCLEAN=1 -P ${CGS_ASSEMBLE})
    if(DS_DIST)
        set(CGS_PACKAGE ${CMAKE_BINARY_DIR}/package)
        set(CGS_ZIP ${CMAKE_BINARY_DIR}/GlobalStormFix-${PROJECT_VERSION}.zip)
        list(APPEND CGS_TREE_COMMANDS
            COMMAND ${CMAKE_COMMAND} -DDEST=${CGS_PACKAGE} -DCLEAN=1
                "-DFILES=${CGS_TREE}@${CMAKE_CURRENT_SOURCE_DIR}/dist/GlobalStormFix-README.txt|GlobalStormFix-README.txt"
                -P ${CGS_ASSEMBLE}
            COMMAND ${CMAKE_COMMAND} -E rm -f ${CGS_ZIP}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CGS_PACKAGE} ${CMAKE_COMMAND} -E tar cf ${CGS_ZIP} --format=zip .)
        list(APPEND CGS_TREE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dist/GlobalStormFix-README.txt)
    endif()
    if(CGS_GAME_DIR)
        list(APPEND CGS_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CGS_GAME_DIR} "-DFILES=${CGS_TREE_FILES}"
            -P ${CGS_ASSEMBLE})
    endif()
    set(CGS_TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
    add_custom_command(OUTPUT ${CGS_TREE_STAMP}
        ${CGS_TREE_COMMANDS}
        COMMAND ${CMAKE_COMMAND} -E touch ${CGS_TREE_STAMP}
        DEPENDS ${CGS_TREE_DEPENDS} ${CGS_ASSEMBLE}
        COMMENT "GlobalStormFix PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>"
        VERBATIM)
    add_custom_target(cgs_pse_tree ALL DEPENDS ${CGS_TREE_STAMP})
endif()
