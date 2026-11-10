# vcpkg's built-in triplet of the same name, plus one rule: dependencies are compiled by the same
# Visual Studio and MSVC version as the engine. Otherwise vcpkg picks the newest installed one, and
# a C++ library built by a newer MSVC can't be linked by an older one. Presets add this folder
# with VCPKG_OVERLAY_TRIPLETS.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Set by vcvars, which CLion, the Visual Studio command prompts and CI all run before building.
if(DEFINED ENV{VCToolsVersion} AND DEFINED ENV{VSINSTALLDIR})
    # vcpkg wants the first two parts, such as 14.38.
    string(REGEX MATCH "^[0-9]+[.][0-9]+" VCPKG_PLATFORM_TOOLSET_VERSION "$ENV{VCToolsVersion}")

    # vcpkg compares the folder as written: backslashes, and no trailing one.
    set(VCPKG_VISUAL_STUDIO_PATH "$ENV{VSINSTALLDIR}")
    string(REGEX REPLACE "[\\\\/]+$" "" VCPKG_VISUAL_STUDIO_PATH "${VCPKG_VISUAL_STUDIO_PATH}")
endif()
