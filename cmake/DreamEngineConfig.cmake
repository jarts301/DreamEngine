# find_package(DreamEngine) for a game in a repository of its own (ADR 0045).
#
# The SDK is the engine's sources: an installed copy, made with `cmake --install`, or a checkout.
# This file adds them to the game's build, which then compiles the engine with the game's own
# compiler, platform and module lists, just as the samples inside the engine are built. A game
# names the SDK with DreamEngine_DIR, its `cmake` folder, as `dream new` sets up its presets to do:
#
#     cmake_minimum_required(VERSION 3.25)
#     project(my_game LANGUAGES CXX)
#     find_package(DreamEngine 0.1 REQUIRED)
#     dream_add_game(my_game SOURCES src/my_game.cpp CONFIG dream.toml ASSETS assets)
#
# The game's vcpkg.json must list the engine's libraries, as the template's does.

get_filename_component(DreamEngine_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET dream::runtime)
    if(NOT EXISTS "${DreamEngine_ROOT}/CMakeLists.txt")
        set(DreamEngine_FOUND FALSE)
        set(DreamEngine_NOT_FOUND_MESSAGE
            "${DreamEngine_ROOT} holds no engine sources; DreamEngine_DIR must name the SDK's "
            "cmake folder")
        return()
    endif()
    # Built in the game's build folder, and only what the game uses: the engine's tests and samples
    # are off when it isn't the top-level project.
    add_subdirectory("${DreamEngine_ROOT}" "${CMAKE_BINARY_DIR}/dream_engine" EXCLUDE_FROM_ALL)
endif()

# The engine's settings for the game's own folder, as the engine's CMakeLists.txt sets them for its
# own: no /EHsc, which conflicts with the engine's no-exceptions setting, and position-independent
# code, which Android's shared libraries need.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DreamEngine_FOUND TRUE)
