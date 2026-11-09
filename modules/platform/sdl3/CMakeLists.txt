# GLOBAL because the game's final link needs SDL's targets, and it happens in another directory.
find_package(SDL3 CONFIG REQUIRED GLOBAL)

dream_add_module(platform sdl3 SOURCES src/sdl_platform.cpp LIBRARIES SDL3::SDL3)
