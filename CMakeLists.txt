cmake_minimum_required(VERSION 3.22)

project(DreamEngine VERSION 0.1.0 LANGUAGES C CXX)

# This branch builds on Linux only, offline, for machines with no GPU, display or sound card, and
# shows every game and test in a web browser. See docs/adr/0039-offline-linux-web-build.md.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This branch of DreamEngine builds on Linux only (ADR 0039).")
endif()

option(DREAM_BUILD_TESTS "Build the engine's tests" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_BUILD_SAMPLES "Build the sample programs" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Which profiler records the zones is chosen in the project configuration. OFF removes the zones
# from the engine and games altogether, for builds that ship. See docs/adr/0015-profiling.md.
option(DREAM_PROFILING "Compile profiling zones into the engine and games" ON)

# The software renderer is slow without optimization, so a build that doesn't say otherwise is a
# release build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release or RelWithDebInfo" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/DreamTargets.cmake)
include(cmake/DreamModules.cmake)
include(cmake/DreamGame.cmake)

# Header-only libraries kept in the repository, so nothing is downloaded (ADR 0039).
add_subdirectory(third_party)

if(DREAM_BUILD_TESTS)
    enable_testing()
    add_test(NAME dream_module_config_reader
        COMMAND ${CMAKE_COMMAND} -P "${PROJECT_SOURCE_DIR}/cmake/tests/module_config_test.cmake")
endif()

add_subdirectory(core)
add_subdirectory(interfaces)
add_subdirectory(libraries)
add_subdirectory(runtime)

# Tools that run on the build machine: packing a game's assets, and the hub, the web page that
# starts the games and runs the tests.
add_subdirectory(tools/pack)
add_subdirectory(tools/hub)

if(DREAM_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# The games the hub can start, one line each: <name>|<program>|<title>.
get_property(dream_hub_games GLOBAL PROPERTY DREAM_HUB_GAMES)
list(JOIN dream_hub_games "\n" dream_hub_games)
file(WRITE "${CMAKE_BINARY_DIR}/hub_games.txt" "${dream_hub_games}\n")
