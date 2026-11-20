# ozz-animation, which vcpkg's own registry doesn't have (ADR 0036). Builds the base, runtime and
# offline libraries, without ozz's samples, tools, tests or FBX and glTF importers: the engine reads
# glTF itself, with cgltf, and builds ozz's animations from it as models load.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO guillaumeblanc/ozz-animation
    REF "${VERSION}"
    SHA512 85e6c1b5e693c2f044815b92cb0896fb7f7d495d91cb9ff55aef4615483c33c3e4ef7232f84aa6539c92aed0e9d842fcc448712166a0eab2b252525196caff90
    HEAD_REF master
)

# ozz treats its own warnings as errors, which compilers newer than its release can trip over.
vcpkg_replace_string("${SOURCE_PATH}/build-utils/cmake/compiler_settings.cmake"
    "set(CMAKE_COMPILE_WARNING_AS_ERROR ON)" "")

if(VCPKG_CRT_LINKAGE STREQUAL "dynamic")
    set(msvc_runtime_dll ON)
else()
    set(msvc_runtime_dll OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dozz_build_tools=OFF
        -Dozz_build_fbx=OFF
        -Dozz_build_gltf=OFF
        -Dozz_build_data=OFF
        -Dozz_build_samples=OFF
        -Dozz_build_howtos=OFF
        -Dozz_build_tests=OFF
        -Dozz_build_postfix=OFF
        -Dozz_build_msvc_rt_dll=${msvc_runtime_dll}
)
vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/doc")
# ozz installs no CMake package, so this port provides one.
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/ozz-animation-config.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
