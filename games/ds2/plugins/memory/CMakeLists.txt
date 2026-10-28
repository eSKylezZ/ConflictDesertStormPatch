# ds2fix.memory (games/ds2/plugins/memory): picked up by games/ds2/CMakeLists.txt's plugin glob (DS2_PSE_PLUGIN).
# Self-contained (SDK header + games/ds2/src/pse/ds2fix_memory.h, its interface). Docs: docs/sdk/reference/game-ds2.md
# "ds2fix.memory".
pse_add_plugin(ds2fix_memory SOURCES plugin.cpp ${DS2_SOURCE_DIR}/src/pse/ds2fix_memory.h MANIFEST ds2fix.memory.pse)
target_include_directories(ds2fix_memory PRIVATE ${DS2_SOURCE_DIR}/src/pse)
target_compile_definitions(ds2fix_memory PRIVATE DS_VERSION="${PROJECT_VERSION}")
target_link_options(ds2fix_memory PRIVATE /SAFESEH:NO)
