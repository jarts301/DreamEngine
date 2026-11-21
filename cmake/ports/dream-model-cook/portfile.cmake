# The engine's own model cook tool (ADR 0042), from this repository rather than a download: vcpkg
# builds it for the build machine, so builds that cross-compile, such as Android's, can run it. Its
# sources are outside this port, so vcpkg can't see them change: the version in vcpkg.json must be
# raised when they do. A tool left behind is harmless, as the runtime checks what it made and makes
# the versions itself when they don't match.
set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../../.." ABSOLUTE)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/tools/model_cook"
    OPTIONS "-DDREAM_ENGINE_ROOT=${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_copy_tools(TOOL_NAMES dream_model_cook
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/dream-model-cook" AUTO_CLEAN)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
