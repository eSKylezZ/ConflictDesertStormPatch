cmake_minimum_required(VERSION 3.21)
project(VietnamFix VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Vietnam.exe is 32-bit: configure with -A Win32 (use the CMake presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT, no redist needed

# Folder containing Vietnam.exe; when set, every build copies the output there.
set(DS_GAME_DIR "" CACHE PATH "Conflict: Vietnam install folder (optional deploy target)")
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
    src/features/frame.cpp
    src/features/frontend.cpp
    src/features/probe.cpp
    src/features/mods.cpp
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
    set(DIST_ZIP ${CMAKE_BINARY_DIR}/VietnamFix-${PROJECT_VERSION}.zip)
    set_property(TARGET dinput8 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dist/VietnamFix-README.txt")
    add_custom_command(TARGET dinput8 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${CMAKE_CURRENT_SOURCE_DIR}/dist/VietnamFix-README.txt" "${DIST_DIR}"
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

# ---- VietnamFix on the Pivotal Script Extender (PSE), 2026-10-24 ---------------------------------------------------
# The PSE build (VietnamFix as the host plugin cvfix.dll + the PSE runtime, packaged; section below). With it off
# (default) this file builds only the standalone dinput8.dll above - both keep working (DS2's precedent,
# games/ds2/CMakeLists.txt). The PSE install tree (docs/sdk/layout.md, docs/sdk/reference/game-cv.md):
#   dinput8.dll                     the PSE shim (game-neutral, pse/shim)
#   PSE\pse_loader.dll              the loader (pse/loader + pse/loader_dll)
#   PSE\Games\pse_cv.dll            the CV game layer (pse/games/cv)
#   PSE\Plugins\cvfix.dll + .pse    VietnamFix as the host plugin (src/pse/cvfix.cpp): widescreen, Mods folder
# From games/cv: cmake -S . -B build-pse -A Win32 -DCV_PSE_PLUGIN=ON && cmake --build build-pse --config RelWithDebInfo
# or from the repo root: cmake --preset cv-pse && cmake --build --preset cv-pse (release package: cv-pse-dist).
option(CV_PSE_PLUGIN "Also build cvfix.dll, VietnamFix as a PSE host plugin, and its PSE install tree / package" OFF)
set(PSE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The PSE repo root (pse/, games/ds1/src/core/patch.cpp)")
set(CV_GAME_DIR "" CACHE PATH "Conflict: Vietnam install folder: the PSE tree is copied there (replaces dinput8.dll)")
if(CV_PSE_PLUGIN)
    get_filename_component(PSE_REPO_ABS "${PSE_REPO}" ABSOLUTE)
    if(NOT EXISTS "${PSE_REPO_ABS}/pse/hostlink/hostlink.cpp")
        message(FATAL_ERROR "CV_PSE_PLUGIN: PSE repo not found at ${PSE_REPO_ABS} (set PSE_REPO)")
    endif()
    if(CV_GAME_DIR AND DS_GAME_DIR)
        message(FATAL_ERROR "DS_GAME_DIR (standalone dinput8.dll) and CV_GAME_DIR (PSE tree) both set - pick one")
    endif()
    set(PSE_SRC "${PSE_REPO_ABS}/pse")
    set(CV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    # cv_ship_target(<target> <description> <file name>): dev = PDB, DS_DIST = no PDB / timestamp / Rich header, both
    # with a version resource (DS1's ds_ship_target with VietnamFix's version).
    function(cv_ship_target t description filename)
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
        configure_file(${CV_SOURCE_DIR}/src/pse/version_pse.rc.in ${CMAKE_BINARY_DIR}/version_${t}.rc @ONLY)
        target_sources(${t} PRIVATE ${CMAKE_BINARY_DIR}/version_${t}.rc)
    endfunction()

    # The PSE runtime, in its own output folders (the shim is also called dinput8.dll / dinput8.lib).
    file(GLOB CV_PSE_CORE_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/loader/*.cpp)
    add_library(cv_pse_core STATIC ${CV_PSE_CORE_SOURCES})
    target_include_directories(cv_pse_core PUBLIC ${PSE_SRC}/include PRIVATE ${PSE_SRC}/loader)
    target_compile_definitions(cv_pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cv_pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(cv_pse_core PUBLIC advapi32 bcrypt)
    add_library(cv_pse_hostlink STATIC ${PSE_SRC}/hostlink/hostlink.cpp ${PSE_SRC}/loader/compat.cpp
        ${PSE_SRC}/loader/x86len.cpp ${PSE_SRC}/loader/settings_migrate.cpp ${PSE_SRC}/loader/util.cpp)
    target_include_directories(cv_pse_hostlink PUBLIC ${PSE_SRC}/include ${PSE_SRC}/loader ${PSE_SRC}/hostlink)
    target_compile_definitions(cv_pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cv_pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
    target_link_libraries(cv_pse_hostlink PUBLIC advapi32 bcrypt)

    add_library(cv_pse_loader SHARED ${PSE_SRC}/loader_dll/loader_main.cpp)
    target_include_directories(cv_pse_loader PRIVATE ${PSE_SRC}/loader)
    target_link_libraries(cv_pse_loader PRIVATE cv_pse_core)
    add_library(cv_pse_shim SHARED ${PSE_SRC}/shim/shim.cpp ${PSE_SRC}/shim/proxy_dinput8.cpp ${PSE_SRC}/shim/dinput8.def)
    target_include_directories(cv_pse_shim PRIVATE ${PSE_SRC}/include ${PSE_SRC}/loader)
    file(GLOB CV_PSE_LAYER_SOURCES CONFIGURE_DEPENDS ${PSE_SRC}/games/cv/*.cpp ${PSE_SRC}/games/cv/*.h)
    add_library(cv_pse_layer SHARED ${CV_PSE_LAYER_SOURCES} ${PSE_SRC}/loader/ids.cpp)
    target_include_directories(cv_pse_layer PRIVATE ${PSE_SRC}/games/cv ${PSE_SRC}/ids)   # generated/pse_ids_cv.h
    target_link_libraries(cv_pse_layer PRIVATE cv_pse_hostlink)
    foreach(t cv_pse_loader cv_pse_shim cv_pse_layer)
        target_link_options(${t} PRIVATE /SAFESEH:NO)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_runtime_lib")
    endforeach()
    set_target_properties(cv_pse_loader PROPERTIES OUTPUT_NAME pse_loader)
    set_target_properties(cv_pse_shim PROPERTIES OUTPUT_NAME dinput8)
    set_target_properties(cv_pse_layer PROPERTIES OUTPUT_NAME pse_cv)
    cv_ship_target(cv_pse_loader "Pivotal Script Extender loader" pse_loader.dll)
    cv_ship_target(cv_pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
    cv_ship_target(cv_pse_layer "Pivotal Script Extender - Conflict: Vietnam game layer" pse_cv.dll)

    # cvfix.dll + cvfix.pse: VietnamFix as the host plugin (src/pse/cvfix.cpp): widescreen + Mods folder, reusing
    # features/widescreen.cpp, features/mods.cpp, features/devbg.cpp, features/graphics.cpp UNCHANGED (src/pse/shim
    # first on the include path replaces core/patch.h / core/pse_host.h for this target - see those headers).
    add_library(cvfix SHARED
        src/pse/cvfix.cpp src/pse/cvfix.pse
        src/core/log.cpp
        src/features/gameexe.cpp src/features/widescreen.cpp src/features/mods.cpp
        src/features/devbg.cpp src/features/graphics.cpp
        ${PSE_REPO_ABS}/games/ds1/src/core/patch.cpp)
    set_source_files_properties(src/pse/cvfix.pse PROPERTIES HEADER_FILE_ONLY ON)
    target_include_directories(cvfix PRIVATE src/pse/shim src)
    target_compile_definitions(cvfix PRIVATE DS_VERSION="${PROJECT_VERSION}"
        "CV_PSE_PATCH_H=\"${PSE_REPO_ABS}/games/ds1/src/core/patch.h\"")
    target_link_libraries(cvfix PRIVATE cv_pse_hostlink d3d9 advapi32 bcrypt ole32)
    target_link_options(cvfix PRIVATE /SAFESEH:NO)
    set_target_properties(cvfix PROPERTIES OUTPUT_NAME cvfix PREFIX "")
    cv_ship_target(cvfix "VietnamFix - Conflict: Vietnam enhancement patch (PSE host plugin)" cvfix.dll)
    add_custom_command(TARGET cvfix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/pse/cvfix.pse" "$<TARGET_FILE_DIR:cvfix>"
        COMMENT "cvfix.pse next to cvfix.dll")

    # The install tree (assemble.cmake: "<src>|<dest>@..."; "?" = optional - the PDBs).
    set(CV_TREE "$<TARGET_FILE:cv_pse_shim>|dinput8.dll@$<TARGET_FILE:cv_pse_loader>|PSE/pse_loader.dll")
    string(APPEND CV_TREE "@$<TARGET_FILE:cv_pse_layer>|PSE/Games/pse_cv.dll")
    string(APPEND CV_TREE "@$<TARGET_FILE:cvfix>|PSE/Plugins/cvfix.dll")
    string(APPEND CV_TREE "@${CMAKE_CURRENT_SOURCE_DIR}/src/pse/cvfix.pse|PSE/Plugins/cvfix.pse")
    set(CV_TREE_PDBS "?$<TARGET_PDB_FILE:cv_pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:cv_pse_loader>|PSE/pse_loader.pdb")
    string(APPEND CV_TREE_PDBS "@?$<TARGET_PDB_FILE:cv_pse_layer>|PSE/Games/pse_cv.pdb")
    string(APPEND CV_TREE_PDBS "@?$<TARGET_PDB_FILE:cvfix>|PSE/Plugins/cvfix.pdb")
    set(CV_TREE_DEPENDS cv_pse_shim cv_pse_loader cv_pse_layer cvfix ${CMAKE_CURRENT_SOURCE_DIR}/src/pse/cvfix.pse)
    set(CV_ASSEMBLE ${PSE_SRC}/cmake/assemble.cmake)
    if(DS_DIST)
        set(CV_TREE_FILES "${CV_TREE}")
    else()
        set(CV_TREE_FILES "${CV_TREE}@${CV_TREE_PDBS}")
    endif()
    set(CV_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree/$<CONFIG> "-DFILES=${CV_TREE_FILES}"
        -DCLEAN=1 -P ${CV_ASSEMBLE})
    if(DS_DIST)
        set(CV_PACKAGE ${CMAKE_BINARY_DIR}/package)
        set(CV_ZIP ${CMAKE_BINARY_DIR}/VietnamFix-${PROJECT_VERSION}.zip)
        list(APPEND CV_TREE_COMMANDS
            COMMAND ${CMAKE_COMMAND} -DDEST=${CV_PACKAGE} -DCLEAN=1
                "-DFILES=${CV_TREE}@${CMAKE_CURRENT_SOURCE_DIR}/dist/VietnamFix-README.txt|VietnamFix-README.txt"
                -P ${CV_ASSEMBLE}
            COMMAND ${CMAKE_COMMAND} -E rm -f ${CV_ZIP}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CV_PACKAGE} ${CMAKE_COMMAND} -E tar cf ${CV_ZIP} --format=zip .)
        list(APPEND CV_TREE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dist/VietnamFix-README.txt)
    endif()
    if(CV_GAME_DIR)
        list(APPEND CV_TREE_COMMANDS COMMAND ${CMAKE_COMMAND} -DDEST=${CV_GAME_DIR} "-DFILES=${CV_TREE_FILES}"
            -P ${CV_ASSEMBLE})
    endif()
    set(CV_TREE_STAMP ${CMAKE_BINARY_DIR}/tree/$<CONFIG>.stamp)
    add_custom_command(OUTPUT ${CV_TREE_STAMP}
        ${CV_TREE_COMMANDS}
        COMMAND ${CMAKE_COMMAND} -E touch ${CV_TREE_STAMP}
        DEPENDS ${CV_TREE_DEPENDS} ${CV_ASSEMBLE}
        COMMENT "VietnamFix PSE tree in ${CMAKE_BINARY_DIR}/tree/<config>"
        VERBATIM)
    add_custom_target(cv_pse_tree ALL DEPENDS ${CV_TREE_STAMP})
endif()
