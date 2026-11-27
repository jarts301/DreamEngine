# The engine's own atlas cook tool (ADR 0052), from this repository rather than a download: vcpkg
# builds it for the build machine, so builds that cross-compile, such as Android's, can run it. Its
# sources are outside this port, so vcpkg can't see them change: the version in vcpkg.json must be
# raised when they do.
set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../../.." ABSOLUTE)

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}/tools/atlas_cook")
vcpkg_cmake_install()
vcpkg_copy_tools(TOOL_NAMES dream_atlas_cook
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/dream-atlas-cook" AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
