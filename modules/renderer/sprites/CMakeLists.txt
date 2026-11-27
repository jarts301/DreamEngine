# The renderer for games without a 3D scene (ADR 0053), built from the forward renderer's source with
# DREAM_SPRITES_RENDERER: it draws the 2D world's sprites, debug lines and the interface, and ships
# only their shaders.
set(forward_dir "${CMAKE_CURRENT_SOURCE_DIR}/../forward")
dream_add_module(renderer sprites
    SOURCES ${forward_dir}/src/forward_renderer.cpp
    SHADERS
        ${forward_dir}/shaders/debug_lines.slang
        ${forward_dir}/shaders/sprites.slang
        ${forward_dir}/shaders/ui.slang
)
target_compile_definitions(dream_module_renderer_sprites PRIVATE DREAM_SPRITES_RENDERER)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_renderer_sprites_tests
        SOURCES
            ${forward_dir}/tests/test_main.cpp
            tests/sprites_renderer_test.cpp
        LIBRARIES
            dream::module_renderer_sprites
    )
    target_include_directories(dream_renderer_sprites_tests PRIVATE ${forward_dir}/tests)
endif()
