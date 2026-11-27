# The atlas cook tool (ADR 0052), which packs a folder of images into one at build time. It needs
# only stb, and builds either inside the engine's build, for PC builds, or on its own, as the vcpkg
# port in cmake/ports/dream-atlas-cook does for builds that cross-compile, such as Android's.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(dream_atlas_cook LANGUAGES CXX)
endif()

find_path(DREAM_ATLAS_STB_INCLUDE_DIR stb_rect_pack.h REQUIRED)

add_executable(dream_atlas_cook atlas_cook.cpp)
target_compile_features(dream_atlas_cook PRIVATE cxx_std_20)
target_include_directories(dream_atlas_cook SYSTEM PRIVATE ${DREAM_ATLAS_STB_INCLUDE_DIR})
# stb's sources are the C kind the engine's warnings don't hold to, so the engine's settings apply
# only inside its build, where they are known to pass; on its own, the same essentials.
if(COMMAND dream_configure_target)
    dream_configure_target(dream_atlas_cook)
elseif(MSVC)
    target_compile_options(dream_atlas_cook PRIVATE /permissive- /utf-8 /EHs-c- /GR-)
    target_compile_definitions(dream_atlas_cook PRIVATE _HAS_EXCEPTIONS=0 NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(dream_atlas_cook PRIVATE -fno-exceptions -fno-rtti)
endif()

install(TARGETS dream_atlas_cook RUNTIME DESTINATION bin)
