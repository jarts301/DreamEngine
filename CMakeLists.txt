cmake_minimum_required(VERSION 3.25)

project(DreamEngine VERSION 0.1.0 LANGUAGES CXX)

option(DREAM_BUILD_TESTS "Build the engine's tests" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_BUILD_SAMPLES "Build the sample programs" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Which profiler records the zones is chosen in the project configuration. OFF removes the zones
# from the engine and games altogether, for builds that ship. See docs/adr/0015-profiling.md.
# Off ships textures as their source PNGs, for checking whether a problem comes from cooking.
option(DREAM_COOK_TEXTURES "Cook game textures into compressed KTX2 files" ON)
option(DREAM_COOK_MODELS "Make models' levels of detail at build time (ADR 0042)" ON)
option(DREAM_PROFILING "Compile profiling zones into the engine and games" ON)
# The editor (ADR 0046), a program like a game, built where the build is for the build machine.
option(DREAM_BUILD_EDITOR "Build the editor" ${PROJECT_IS_TOP_LEVEL})
# Off when a game builds the engine through find_package, so the game's own install doesn't copy it.
option(DREAM_INSTALL_SDK "Install the SDK with cmake --install (ADR 0045)" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# CMake adds /EHsc by default. Removing it here avoids a flag conflict with the no-exceptions
# setting in dream_configure_target.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

include(cmake/DreamTargets.cmake)
include(cmake/DreamModules.cmake)
include(cmake/DreamGame.cmake)

if(DREAM_BUILD_TESTS)
    enable_testing()
    find_package(doctest CONFIG QUIET)
    if(NOT doctest_FOUND)
        message(FATAL_ERROR
            "doctest was not found, which usually means this build is not using vcpkg. "
            "Configure with a CMake preset and make sure VCPKG_ROOT is set; see README.md.")
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_module_config_reader
            COMMAND ${CMAKE_COMMAND} -P "${PROJECT_SOURCE_DIR}/cmake/tests/module_config_test.cmake")
    endif()
endif()

add_subdirectory(core)
add_subdirectory(interfaces)
add_subdirectory(libraries)
add_subdirectory(runtime)

# Tools that run on the build machine, such as packing a PC game's assets, and `dream`, which makes
# new game projects.
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tools/pack)
    add_subdirectory(tools/dream)
    if(DREAM_BUILD_EDITOR)
        add_subdirectory(tools/editor)
    endif()
    # Cross-compiled builds, such as Android's, take it from vcpkg instead (ADR 0042).
    if(DREAM_COOK_MODELS)
        add_subdirectory(tools/model_cook)
    endif()
endif()

if(DREAM_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# The SDK (ADR 0045): `cmake --install <build> --prefix <folder>` copies the engine's sources there,
# which a game in its own repository builds through find_package(DreamEngine), and the `dream` tool
# into its bin folder. The engine's own tests come along but stay off in a game's build; the samples
# and build folders don't come.
if(DREAM_INSTALL_SDK)
    install(FILES CMakeLists.txt vcpkg.json LICENSE README.md DESTINATION .)
    install(DIRECTORY cmake core docs interfaces libraries modules runtime templates tools
        DESTINATION .
        PATTERN "__pycache__" EXCLUDE
        PATTERN "*.pyc" EXCLUDE)
endif()
