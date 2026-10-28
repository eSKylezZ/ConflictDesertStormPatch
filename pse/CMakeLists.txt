# Pivotal Script Extender (docs/internal/DESIGN.md, layout docs/sdk/layout.md). Phase 2 layout:
#   pse_shim     -> dinput8.dll               proxy + entry-point hook, starts the loader (shim/)
#   pse_loader   -> PSE\pse_loader.dll        pse_core (the ONE copy of the loader services) + start-up (loader_dll/)
#   pse_ds1      -> PSE\Games\pse_ds1.dll     DS1 game layer: detection, pse.game.ds1, core game events + co-save
#                                             (games/ds1/)
#   pse_hostlink    static: loader.h for host plugins / game layers, forwarded through pse.host (hostlink/)
#   pse_core        static: the loader services (loader/*.cpp) - pse_loader.dll and pse_tests only
# Shipped binaries get the release / dev options of the top-level ds_ship_target() when it exists.
file(GLOB PSE_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/loader/*.cpp)
add_library(pse_core STATIC ${PSE_CORE_SOURCES})
target_include_directories(pse_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/loader)
target_compile_definitions(pse_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(pse_core PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(pse_core PUBLIC advapi32 bcrypt)   # bcrypt: SHA-256 (compat.cpp)

# pse_hostlink: what a host plugin / game layer links INSTEAD of pse_core (never both). hostlink.cpp forwards the
# stateful loader.h functions through pse.host; the pure loader files are compiled in as they are.
add_library(pse_hostlink STATIC ${CMAKE_CURRENT_SOURCE_DIR}/hostlink/hostlink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/compat.cpp ${CMAKE_CURRENT_SOURCE_DIR}/loader/compat_local.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/x86len.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/settings_migrate.cpp ${CMAKE_CURRENT_SOURCE_DIR}/loader/util.cpp)
target_include_directories(pse_hostlink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/hostlink)
target_compile_definitions(pse_hostlink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(pse_hostlink PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(pse_hostlink PUBLIC advapi32 bcrypt)

# pse_build (DESIGN.md §12, docs/internal/asset-store.md): the first-run builder - stage 1 = the content-addressed asset
# store. A static library (DesertStormFix's store glue, pse_tests) and pse_build.exe (built in dev AND release builds;
# not shipped in the package yet). No game code: it only reads the install's files.
add_library(pse_build_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/build/store.cpp ${CMAKE_CURRENT_SOURCE_DIR}/build/names.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/build/ds1_archive.cpp ${CMAKE_CURRENT_SOURCE_DIR}/build/sound.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/build/pse_build.h)
target_include_directories(pse_build_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/build)
target_compile_definitions(pse_build_lib PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(pse_build_lib PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(pse_build_lib PUBLIC bcrypt shell32 ole32)
# pse_records (DESIGN.md §13.3-13.4): PSE data files (Lua-table literals, canonical writer) - the records layer's
# format. Byte-identical with the ModKit's conflict_ds_tools/pse_data.py (scripts/records_gate.py).
add_library(pse_records STATIC ${CMAKE_CURRENT_SOURCE_DIR}/records/data.cpp ${CMAKE_CURRENT_SOURCE_DIR}/records/data.h
    ${CMAKE_CURRENT_SOURCE_DIR}/records/forms.cpp ${CMAKE_CURRENT_SOURCE_DIR}/records/forms.h
    ${CMAKE_CURRENT_SOURCE_DIR}/records/placed.cpp ${CMAKE_CURRENT_SOURCE_DIR}/records/placed.h
    ${CMAKE_CURRENT_SOURCE_DIR}/records/scenepatch.cpp ${CMAKE_CURRENT_SOURCE_DIR}/records/scenepatch.h)
target_include_directories(pse_records PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/records)
target_compile_options(pse_records PRIVATE /W4 /permissive- /utf-8 /EHsc $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(pse_build_lib PUBLIC pse_records)
add_executable(pse_build ${CMAKE_CURRENT_SOURCE_DIR}/build/pse_build_main.cpp)
target_compile_definitions(pse_build PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(pse_build PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pse_build PRIVATE pse_build_lib)

# ---- mission2lua: mission DLL -> Lua, the C++ port of ModKit mission2lua.py (docs/internal/mission-translation.md) ----
# Zydis 4.1.0 (MIT, third_party/zydis/VERSION.txt): only the decoder is used (the linker drops the rest). pse_m2l = decoder adapter + flow + emitter + cache,
# linked into pse_build_lib's users (pse_build.exe, dsfix.dll's Dev\MissionSource, pse_tests).
add_library(zydis STATIC ${CMAKE_SOURCE_DIR}/third_party/zydis/Zydis.c)
target_compile_definitions(zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(zydis PRIVATE /W0)
target_include_directories(zydis PUBLIC ${CMAKE_SOURCE_DIR}/third_party/zydis)
file(GLOB PSE_M2L_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/build/mission2lua/m2l_*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/build/mission2lua/m2r_*.cpp)
list(FILTER PSE_M2L_SOURCES EXCLUDE REGEX "m2l_cli\\.cpp$")
add_library(pse_m2l STATIC ${PSE_M2L_SOURCES})
target_include_directories(pse_m2l PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/build/mission2lua)
target_compile_definitions(pse_m2l PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS ZYDIS_STATIC_BUILD
    ZYCORE_STATIC_BUILD)
target_compile_options(pse_m2l PRIVATE /W4 /permissive- /utf-8 $<$<NOT:$<BOOL:${DS_DIST}>>:/Zi>)
target_link_libraries(pse_m2l PUBLIC zydis pse_records bcrypt shell32 ole32)
target_link_libraries(pse_build_lib PUBLIC pse_m2l)
target_sources(pse_build PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/mission2lua/m2l_cli.cpp)
target_link_libraries(pse_build PRIVATE lua_static)   # `missions --check`: compile the generated Lua

# PSE\pse_loader.dll: the loader services + PSELoader_Start (called by the shim at the exe's entry point).
add_library(pse_loader SHARED ${CMAKE_CURRENT_SOURCE_DIR}/loader_dll/loader_main.cpp)
target_include_directories(pse_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/loader)
target_link_libraries(pse_loader PRIVATE pse_core)
set_target_properties(pse_loader PROPERTIES OUTPUT_NAME pse_loader)
target_link_options(pse_loader PRIVATE /SAFESEH:NO)

# dinput8.dll: the shim (proxy exports from shim/dinput8.def). Game-neutral; other games would get another .def.
add_library(pse_shim SHARED ${CMAKE_CURRENT_SOURCE_DIR}/shim/shim.cpp ${CMAKE_CURRENT_SOURCE_DIR}/shim/proxy_dinput8.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/dinput8.def)
target_include_directories(pse_shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/loader)
set_target_properties(pse_shim PROPERTIES OUTPUT_NAME dinput8)
target_link_options(pse_shim PRIVATE /SAFESEH:NO)

# PSE\Games\pse_ds1.dll: the Conflict: Desert Storm game layer.
add_library(pse_ds1 SHARED ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/pse_game_ds1.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/pse_game_ds1.h
    ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/pse_game_ds1_host.h
    ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_events.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_events.h
    ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_saves.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_saves.h
    ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_originals.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1/ds1_originals.h
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/ids.cpp)   # pse.ids: the layer registers the DS1 ID database (also in pse_core)
target_include_directories(pse_ds1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1 ${CMAKE_CURRENT_SOURCE_DIR}/ids)   # generated/pse_ids_ds1.h
target_link_libraries(pse_ds1 PRIVATE pse_hostlink)
set_target_properties(pse_ds1 PROPERTIES OUTPUT_NAME pse_ds1)
target_link_options(pse_ds1 PRIVATE /SAFESEH:NO)

# PSE\Games\pse_ds2.dll: the Conflict: Desert Storm II game layer (docs/sdk/reference/game-ds2.md). Deployed only by
# pse_tree_ds2 below (into DS2_GAME_DIR when set) - never into the DS1 install.
file(GLOB PSE_DS2_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/games/ds2/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/ds2/*.h)
add_library(pse_ds2 SHARED ${PSE_DS2_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/loader/ids.cpp)
target_include_directories(pse_ds2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/games/ds2 ${CMAKE_CURRENT_SOURCE_DIR}/ids)   # generated/pse_ids_ds2.h
target_link_libraries(pse_ds2 PRIVATE pse_hostlink)
set_target_properties(pse_ds2 PROPERTIES OUTPUT_NAME pse_ds2)
target_link_options(pse_ds2 PRIVATE /SAFESEH:NO)

# PSE\Games\pse_cv.dll: the Conflict: Vietnam game layer (docs/sdk/reference/game-cv.md). Deployed only by games/cv's
# own CMakeLists.txt (CV_PSE_PLUGIN) - built here only so pse_tests can load it (test_game_cv.cpp).
file(GLOB PSE_CV_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/games/cv/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/cv/*.h)
add_library(pse_cv SHARED ${PSE_CV_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/loader/ids.cpp)
target_include_directories(pse_cv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/games/cv ${CMAKE_CURRENT_SOURCE_DIR}/ids)   # generated/pse_ids_cv.h
target_link_libraries(pse_cv PRIVATE pse_hostlink)
set_target_properties(pse_cv PROPERTIES OUTPUT_NAME pse_cv)
target_link_options(pse_cv PRIVATE /SAFESEH:NO)

# PSE\Games\pse_cgs.dll: the Conflict: Global Storm / Global Terror game layer (docs/sdk/reference/game-cgs.md).
# Deployed only by games/cgs's own CMakeLists.txt (CGS_PSE_PLUGIN) - built here only so pse_tests can load it
# (test_game_cgs.cpp).
file(GLOB PSE_CGS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/games/cgs/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/games/cgs/*.h)
add_library(pse_cgs SHARED ${PSE_CGS_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/loader/ids.cpp)
target_include_directories(pse_cgs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/games/cgs ${CMAKE_CURRENT_SOURCE_DIR}/ids)   # generated/pse_ids_cgs.h
target_link_libraries(pse_cgs PRIVATE pse_hostlink)
set_target_properties(pse_cgs PROPERTIES OUTPUT_NAME pse_cgs)
target_link_options(pse_cgs PRIVATE /SAFESEH:NO)

# PSE\Games\pse_deniedops.dll: the Conflict: Denied Ops game layer (docs/sdk/reference/game-deniedops.md). Deployed
# only by games/deniedops's own CMakeLists.txt - built here only so pse_tests can load it (test_game_deniedops.cpp).
file(GLOB PSE_DENIEDOPS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/games/deniedops/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/games/deniedops/*.h)
add_library(pse_deniedops SHARED ${PSE_DENIEDOPS_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/loader/ids.cpp)
target_include_directories(pse_deniedops PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/games/deniedops ${CMAKE_CURRENT_SOURCE_DIR}/ids)
target_link_libraries(pse_deniedops PRIVATE pse_hostlink)
set_target_properties(pse_deniedops PROPERTIES OUTPUT_NAME pse_deniedops)
target_link_options(pse_deniedops PRIVATE /SAFESEH:NO)
target_compile_definitions(pse_deniedops PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(pse_deniedops PRIVATE /W4 /permissive- /utf-8)

if(COMMAND ds_ship_target)
    ds_ship_target(pse_loader "Pivotal Script Extender loader" pse_loader.dll)
    ds_ship_target(pse_shim "Pivotal Script Extender shim (dinput8 proxy)" dinput8.dll)
    ds_ship_target(pse_ds1 "Pivotal Script Extender - Conflict: Desert Storm game layer" pse_ds1.dll)
    ds_ship_target(pse_ds2 "Pivotal Script Extender - Conflict: Desert Storm II game layer" pse_ds2.dll)
    ds_ship_target(pse_cv "Pivotal Script Extender - Conflict: Vietnam game layer" pse_cv.dll)
    ds_ship_target(pse_cgs "Pivotal Script Extender - Conflict: Global Storm game layer" pse_cgs.dll)
else()
    foreach(t pse_loader pse_shim pse_ds1 pse_ds2 pse_cv pse_cgs)
        target_compile_definitions(${t} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

# pse_add_plugin(): builds a plugin DLL against the SDK header only (pse/cmake/PSEPlugin.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PSEPlugin.cmake)

# Example plugins (docs/sdk/guides/first-plugin.md) -> ${CMAKE_BINARY_DIR}/pse_plugins/<config>/. Also the test
# plugins of pse_tests (test_plugins.cpp). Not built for the release package.
if(NOT DS_DIST)
    pse_add_plugin(pse_example_hello SOURCES examples/hello/hello.cpp examples/hello/example_hello.h
        MANIFEST examples/hello/example.hello.pse)
    pse_add_plugin(pse_example_hello_client SOURCES examples/hello_client/hello_client.cpp
        MANIFEST examples/hello_client/example.hello_client.pse)
    target_include_directories(pse_example_hello_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello)
    pse_add_plugin(pse_example_bad_crash SOURCES examples/bad_crash/bad_crash.cpp
        MANIFEST examples/bad_crash/example.bad_crash.pse)
    # Late-crash test plugins (one source, four names): fault in PostLoad, a frame event, a task, other callbacks.
    foreach(where postload frame task callbacks)
        pse_add_plugin(pse_example_crash_${where} SOURCES examples/crash_later/crash_later.cpp
            MANIFEST examples/crash_later/example.crash_${where}.pse)
    endforeach()
    target_compile_definitions(pse_example_crash_postload PRIVATE CRASH_NAME="example.crash_postload" CRASH_MODE=1)
    target_compile_definitions(pse_example_crash_frame PRIVATE CRASH_NAME="example.crash_frame" CRASH_MODE=2)
    target_compile_definitions(pse_example_crash_task PRIVATE CRASH_NAME="example.crash_task" CRASH_MODE=3)
    target_compile_definitions(pse_example_crash_callbacks PRIVATE CRASH_NAME="example.crash_callbacks" CRASH_MODE=4)
    # A SIMULATION-flagged plugin for the online plugin-set check (docs/sdk/online-compat.md).
    pse_add_plugin(pse_example_sim_rules SOURCES examples/sim_rules/sim_rules.cpp
        MANIFEST examples/sim_rules/example.sim_rules.pse)
    pse_add_plugin(pse_example_sim_fingerprint SOURCES examples/sim_fingerprint/sim_fingerprint.cpp
        MANIFEST examples/sim_fingerprint/example.sim_fingerprint.pse)
    set(PSE_EXAMPLE_PLUGINS pse_example_hello pse_example_hello_client pse_example_bad_crash pse_example_crash_postload
        pse_example_crash_frame pse_example_crash_task pse_example_crash_callbacks pse_example_sim_rules
        pse_example_sim_fingerprint)
    foreach(t IN LISTS PSE_EXAMPLE_PLUGINS)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
    # pse.lua example (docs/sdk/reference/lua.md): Lua functions for mission scripts (also loaded by test_lua_iface.cpp).
    pse_add_plugin(pse_example_lua_native SOURCES examples/lua_native/lua_native.cpp
        MANIFEST examples/lua_native/example.lua_native.pse)
    target_compile_options(pse_example_lua_native PRIVATE /W4 /permissive- /utf-8)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_example_lua_native)
    # The game layer's core events / messages + a co-save chunk, logged to a file next to the DLL (docs/sdk/reference/
    # messaging.md). Needs no DesertStormFix: tests/live/test_layer_without_dsfix.py runs it without dsfix.dll.
    pse_add_plugin(pse_example_events_logger SOURCES examples/events_logger/events_logger.cpp
        MANIFEST examples/events_logger/example.events_logger.pse)
    target_compile_definitions(pse_example_events_logger PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pse_example_events_logger PRIVATE /W4 /permissive- /utf-8)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_example_events_logger)
    # TEST ONLY: game off-screen without DesertStormFix (boot a level, save / load / reload) for the same live test.
    pse_add_plugin(pse_test_layer_harness SOURCES tests/layer_harness/layer_harness.cpp
        MANIFEST tests/layer_harness/test.layer_harness.pse)
    target_compile_definitions(pse_test_layer_harness PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pse_test_layer_harness PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(pse_test_layer_harness PRIVATE ole32)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_test_layer_harness)
    # A HOST plugin for test_layout.cpp (pse.host through pse_hostlink - not an SDK example: host plugins are PSE's
    # own, docs/sdk/reference/plugins.md "Host plugins"). Never deployed into a game.
    pse_add_plugin(pse_test_host SOURCES tests/host_plugin/host_plugin.cpp MANIFEST tests/host_plugin/test.host.pse)
    target_link_libraries(pse_test_host PRIVATE pse_hostlink)
    target_compile_definitions(pse_test_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pse_test_host PRIVATE /W4 /permissive- /utf-8)
    # The same as "dsfix" (the only host name the DS1 game layer allows) for the start-up test, in its own folder so it
    # never meets the real dsfix.dll.
    pse_add_plugin(pse_test_host_dsfix SOURCES tests/host_plugin/host_plugin.cpp MANIFEST tests/host_plugin/fake/dsfix.pse
        OUTPUT_DIR ${CMAKE_BINARY_DIR}/pse_test_plugins)
    target_link_libraries(pse_test_host_dsfix PRIVATE pse_hostlink)
    target_compile_definitions(pse_test_host_dsfix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        TEST_HOST_NAME="dsfix")
    target_compile_options(pse_test_host_dsfix PRIVATE /W4 /permissive- /utf-8)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_test_host pse_test_host_dsfix)
    # pse.ui example (docs/sdk/reference/ui.md): a setting of every type (automatic settings page + launcher rows), an
    # overlay text and a pop-up.
    pse_add_plugin(pse_example_ui_demo SOURCES examples/ui_demo/ui_demo.cpp MANIFEST examples/ui_demo/example.ui_demo.pse)
    target_compile_options(pse_example_ui_demo PRIVATE /W4 /permissive- /utf-8)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_example_ui_demo)

    # ---- Desert Storm II (docs/sdk/reference/game-ds2.md) ----
    # TEST ONLY: DS2 off-screen / unfocused / muted, booting a level, save / load (test.ds2_harness, never shipped).
    pse_add_plugin(pse_test_ds2_harness SOURCES tests/ds2_harness/ds2_harness.cpp
        MANIFEST tests/ds2_harness/test.ds2_harness.pse)
    target_compile_definitions(pse_test_ds2_harness PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pse_test_ds2_harness PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(pse_test_ds2_harness PRIVATE ole32)
    list(APPEND PSE_EXAMPLE_PLUGINS pse_test_ds2_harness)
    # The DS2 install tree: ${CMAKE_BINARY_DIR}/tree-ds2/<config>/ = dinput8.dll (the same shim), PSE\pse_loader.dll (the
    # same loader), PSE\Games\pse_ds2.dll and the game-neutral example plugins - the SAME plugin DLLs as in the DS1 tree.
    # Copied into DS2_GAME_DIR when set (it replaces that folder's dinput8.dll: DesertStorm2Fix's own - back it up).
    set(DS2_GAME_DIR "" CACHE PATH "Conflict: Desert Storm II install folder (optional deploy target of pse_tree_ds2)")
    set(DS2_TREE "$<TARGET_FILE:pse_shim>|dinput8.dll@$<TARGET_FILE:pse_loader>|PSE/pse_loader.dll")
    string(APPEND DS2_TREE "@$<TARGET_FILE:pse_ds2>|PSE/Games/pse_ds2.dll")
    string(APPEND DS2_TREE "@?$<TARGET_PDB_FILE:pse_shim>|dinput8.pdb@?$<TARGET_PDB_FILE:pse_loader>|PSE/pse_loader.pdb")
    string(APPEND DS2_TREE "@?$<TARGET_PDB_FILE:pse_ds2>|PSE/Games/pse_ds2.pdb")
    set(DS2_TREE_DEPENDS "")
    foreach(t pse_example_hello pse_example_hello_client pse_example_events_logger)
        get_target_property(stem ${t} OUTPUT_NAME)
        get_target_property(manifest ${t} PSE_MANIFEST)
        string(APPEND DS2_TREE "@$<TARGET_FILE:${t}>|PSE/Plugins/${stem}.dll@${manifest}|PSE/Plugins/${stem}.pse")
        list(APPEND DS2_TREE_DEPENDS ${t} ${manifest})
    endforeach()
    set(DS2_TREE_STAMP ${CMAKE_BINARY_DIR}/tree-ds2/$<CONFIG>.stamp)
    set(DS2_TREE_DEPLOY "")
    if(DS2_GAME_DIR)
        set(DS2_TREE_DEPLOY COMMAND ${CMAKE_COMMAND} -DDEST=${DS2_GAME_DIR} "-DFILES=${DS2_TREE}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/assemble.cmake)
    endif()
    add_custom_command(OUTPUT ${DS2_TREE_STAMP}
        COMMAND ${CMAKE_COMMAND} -DDEST=${CMAKE_BINARY_DIR}/tree-ds2/$<CONFIG> "-DFILES=${DS2_TREE}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/assemble.cmake
        ${DS2_TREE_DEPLOY}
        COMMAND ${CMAKE_COMMAND} -E touch ${DS2_TREE_STAMP}
        DEPENDS pse_shim pse_loader pse_ds2 ${DS2_TREE_DEPENDS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/assemble.cmake
        COMMENT "PSE DS2 tree in ${CMAKE_BINARY_DIR}/tree-ds2/<config>"
        VERBATIM)
    add_custom_target(pse_tree_ds2 ALL DEPENDS ${DS2_TREE_STAMP})
endif()

# pse_tests.exe: unit tests (no game needed) + image tests (DesertStorm.exe mapped at 0x400000, not run).
# Not built for the release package.
if(NOT DS_DIST)
    file(GLOB PSE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(pse_tests ${PSE_TEST_SOURCES})
    target_include_directories(pse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/loader ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/games/ds1/src ${CMAKE_CURRENT_SOURCE_DIR}/ids ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(pse_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(pse_tests PRIVATE /W4 /permissive- /utf-8 /Zi /EHsc)
    target_link_libraries(pse_tests PRIVATE pse_core lua_static)   # lua_static: test_lua_iface.cpp (pse/loader/lua_bridge.h)
    target_link_libraries(pse_tests PRIVATE pse_build_lib)         # test_asset_store.cpp, test_mission2lua.cpp
    target_compile_definitions(pse_tests PRIVATE "PSE_REPO_DIR=\"${CMAKE_SOURCE_DIR}\"")   # test_mission2lua: ../ModKit
    # Keep 0x400000 free for the mapped game image (tests/exe_image.*).
    target_link_options(pse_tests PRIVATE /BASE:0x10000000 /DYNAMICBASE /SAFESEH:NO /DEBUG)
    # test_plugins.cpp loads the example plugins from their output folder.
    add_dependencies(pse_tests ${PSE_EXAMPLE_PLUGINS})
    target_compile_definitions(pse_tests PRIVATE "PSE_PLUGIN_DIR=\"$<TARGET_FILE_DIR:pse_example_hello>\"")
    # test_layout.cpp: the game layer DLL (detection, start-up sequence) and the games/ds1 sources.
    target_include_directories(pse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/games/ds1)
    add_dependencies(pse_tests pse_ds1)
    target_compile_definitions(pse_tests PRIVATE "PSE_GAME_LAYER_DLL=\"$<TARGET_FILE:pse_ds1>\""
        "PSE_FAKE_DSFIX_DIR=\"$<TARGET_FILE_DIR:pse_test_host_dsfix>\"")
    # test_game_ds2.cpp: the DS2 game layer DLL (detection) and the games/ds2 sources.
    add_dependencies(pse_tests pse_ds2)
    target_compile_definitions(pse_tests PRIVATE "PSE_GAME_LAYER_DS2_DLL=\"$<TARGET_FILE:pse_ds2>\"")
    # test_game_cv.cpp: the CV game layer DLL (detection) and the games/cv sources.
    add_dependencies(pse_tests pse_cv)
    target_compile_definitions(pse_tests PRIVATE "PSE_GAME_LAYER_CV_DLL=\"$<TARGET_FILE:pse_cv>\"")
    # test_game_cgs.cpp: the Global Storm game layer DLL (detection) and the games/cgs sources.
    add_dependencies(pse_tests pse_cgs)
    target_compile_definitions(pse_tests PRIVATE "PSE_GAME_LAYER_CGS_DLL=\"$<TARGET_FILE:pse_cgs>\"")
    # test_game_deniedops.cpp: the Denied Ops game layer DLL (detection) + the ids table.
    add_dependencies(pse_tests pse_deniedops)
    target_compile_definitions(pse_tests PRIVATE "PSE_GAME_LAYER_DENIEDOPS_DLL=\"$<TARGET_FILE:pse_deniedops>\"")
    add_test(NAME pse_tests COMMAND pse_tests)
endif()
