# GLOBAL because the game's final link needs SDL's targets, and it happens in another directory.
find_package(SDL3 CONFIG REQUIRED GLOBAL)

dream_add_module(platform sdl3 SOURCES src/sdl_platform.cpp src/http_post.cpp LIBRARIES SDL3::SDL3)
if(WIN32)
    # Platform::post_http's HTTPS client, which comes with Windows (ADR 0043).
    target_link_libraries(dream_module_platform_sdl3 PRIVATE winhttp)
endif()
