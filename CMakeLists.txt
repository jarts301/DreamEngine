cmake_minimum_required(VERSION 3.25)

project(DreamEngine VERSION 0.1.0 LANGUAGES CXX)

option(DREAM_BUILD_TESTS "Build the engine's tests" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_BUILD_SAMPLES "Build the sample programs" ${PROJECT_IS_TOP_LEVEL})
option(DREAM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Which profiler records the zones is chosen in the project configuration. OFF removes the zones
# from the engine and games altogether, for builds that ship. See docs/adr/0015-profiling.md.
option(DREAM_PROFILING "Compile profiling zones into the engine and games" ON)

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

if(DREAM_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
