# ds2fix.limits (games/ds2/plugins/limits): picked up by games/ds2/CMakeLists.txt's plugin glob (DS2_PSE_PLUGIN).
# Self-contained (SDK header + games/ds2/src/pse/ds2fix_memory.h, the memory plugin's interface it asks for heap room).
# Docs: docs/sdk/reference/game-ds2.md "ds2fix.limits", games/ds2/PORTING.md "Engine limits".
pse_add_plugin(ds2fix_limits SOURCES plugin.cpp MANIFEST ds2fix.limits.pse)
target_include_directories(ds2fix_limits PRIVATE ${DS2_SOURCE_DIR}/src/pse)
target_compile_definitions(ds2fix_limits PRIVATE DS_VERSION="${PROJECT_VERSION}")
target_link_options(ds2fix_limits PRIVATE /SAFESEH:NO)
