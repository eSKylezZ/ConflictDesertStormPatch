# pse_add_plugin(<target> SOURCES <files...> MANIFEST <Name.pse> [OUTPUT_DIR <dir>])
#
# Builds a PSE plugin DLL (docs/sdk/guides/first-plugin.md, docs/sdk/reference/plugins.md):
#   - a 32-bit DLL named after the manifest (example.hello.pse -> example.hello.dll), static CRT (no redist, no CRT
#     shared with the loader or other plugins);
#   - the SDK header <pse/pse_api.h> on the include path - nothing else of PSE is linked;
#   - the manifest copied next to the DLL, so the output folder can be dropped into <game>\PSE\Plugins as it is.
# OUTPUT_DIR defaults to ${CMAKE_BINARY_DIR}/pse_plugins (multi-config generators such as Visual Studio add a
# per-configuration subfolder: pse_plugins/Debug, pse_plugins/RelWithDebInfo ...). $<TARGET_FILE_DIR:<target>> is the
# folder in every case.
include_guard(GLOBAL)
set(PSE_SDK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" CACHE INTERNAL "PSE SDK headers")

function(pse_add_plugin target)
    cmake_parse_arguments(ARG "" "MANIFEST;OUTPUT_DIR" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES OR NOT ARG_MANIFEST)
        message(FATAL_ERROR "pse_add_plugin(${target}): SOURCES and MANIFEST are required")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "pse_add_plugin(${target}): PSE plugins are 32-bit - configure with -A Win32")
    endif()
    get_filename_component(manifest "${ARG_MANIFEST}" ABSOLUTE)
    get_filename_component(stem "${manifest}" NAME_WLE)
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/pse_plugins")
    endif()

    add_library(${target} SHARED ${ARG_SOURCES} "${manifest}")
    set_source_files_properties("${manifest}" PROPERTIES HEADER_FILE_ONLY ON)
    target_include_directories(${target} PRIVATE "${PSE_SDK_INCLUDE_DIR}")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${stem}"
        PSE_MANIFEST "${manifest}"   # read by packaging (the top-level install tree)
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIR}"
        # Import libraries / exports files are not needed by anyone: keep them out of the plugin folder.
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pse_plugins_lib")

    # The manifest next to the DLL; a custom target so editing the manifest alone also re-copies it.
    add_custom_target(${target}_manifest
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${manifest}" "$<TARGET_FILE_DIR:${target}>/${stem}.pse"
        DEPENDS "${manifest}"
        COMMENT "PSE manifest ${stem}.pse"
        VERBATIM)
    add_dependencies(${target} ${target}_manifest)
endfunction()
