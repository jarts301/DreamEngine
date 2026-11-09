dream_add_game(dream_cube
    SOURCES src/cube_game.cpp
    CONFIG dream.toml
    SHADERS shaders/cube.slang
    APPLICATION_ID org.dreamengine.cube
    DISPLAY_NAME "Dream Cube"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself. Not a CTest, because build servers have no
    # GPU. Each face's color shows that the coordinate conventions hold.
    dream_add_game(dream_cube_vulkan_check SOURCES src/cube_game.cpp CONFIG tests/vulkan.toml
        SHADERS shaders/cube.slang)

    # The whole path with no display or GPU: compiled shaders load, and every frame is recorded.
    dream_add_game(dream_cube_null SOURCES src/cube_game.cpp CONFIG tests/null.toml
        SHADERS shaders/cube.slang)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_cube_null COMMAND dream_cube_null)
        set_tests_properties(dream_cube_null PROPERTIES
            PASS_REGULAR_EXPRESSION "Using module 'null' for interface 'rhi'.*frames=5"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
