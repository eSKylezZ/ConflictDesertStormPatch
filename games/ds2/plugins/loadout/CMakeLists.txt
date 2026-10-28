# ds2fix.loadout (games/ds2/plugins/loadout): picked up by games/ds2/CMakeLists.txt's plugin glob (DS2_PSE_PLUGIN).
# Self-contained (SDK headers + games/ds2/src/pse/ds2fix_options.h, ds2fix's OPTIONS-row interface). Docs:
# docs/sdk/reference/game-ds2.md "ds2fix.loadout", games/ds2/PORTING.md "Loadout screen".
pse_add_plugin(ds2fix_loadout SOURCES plugin.cpp ${DS2_SOURCE_DIR}/src/pse/ds2fix_options.h ${DS2_SOURCE_DIR}/src/pse/ds2fix_pads.h MANIFEST ds2fix.loadout.pse)
target_include_directories(ds2fix_loadout PRIVATE ${DS2_SOURCE_DIR}/src/pse)
target_compile_definitions(ds2fix_loadout PRIVATE DS_VERSION="${PROJECT_VERSION}")
target_link_libraries(ds2fix_loadout PRIVATE d3d9 gdi32 user32 advapi32)
target_link_options(ds2fix_loadout PRIVATE /SAFESEH:NO)
