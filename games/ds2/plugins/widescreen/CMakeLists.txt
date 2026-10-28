# ds2fix.widescreen (games/ds2/plugins/widescreen): picked up by games/ds2/CMakeLists.txt's plugin glob (DS2_PSE_PLUGIN).
# The feature sources are the standalone build's (games/ds2/src/features); shim/ first on the include path replaces
# core/patch.h (pse.hooks) and core/log.h (pse.log). Docs: docs/sdk/reference/game-ds2.md "DS2 plugins".
set(DS2_FEATURES ${DS2_SOURCE_DIR}/src/features)
pse_add_plugin(ds2fix_widescreen
    SOURCES plugin.cpp patch.cpp
        ${DS2_FEATURES}/widescreen.cpp ${DS2_FEATURES}/hud.cpp ${DS2_FEATURES}/hudfix.cpp ${DS2_FEATURES}/frontend.cpp
        ${DS2_FEATURES}/fontsharp.cpp
        ${PSE_SRC}/loader/x86len.cpp
    MANIFEST ds2fix.widescreen.pse)
target_include_directories(ds2fix_widescreen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${DS2_SOURCE_DIR}/src
    ${DS2_SOURCE_DIR}/src/pse ${PSE_SRC}/loader)
target_compile_definitions(ds2fix_widescreen PRIVATE DS2_WIDESCREEN_PLUGIN DS_VERSION="${PROJECT_VERSION}")
target_link_libraries(ds2fix_widescreen PRIVATE d3d9 advapi32)
target_link_options(ds2fix_widescreen PRIVATE /SAFESEH:NO)
