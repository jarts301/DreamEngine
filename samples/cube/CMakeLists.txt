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
        SHADERS shaders/cube.slang ASSETS tests/reference)

    # The same view on DirectX, which only Windows can build.
    if(WIN32)
        dream_add_game(dream_cube_directx_check SOURCES src/cube_game.cpp
            CONFIG tests/directx.toml SHADERS shaders/cube.slang ASSETS tests/reference)
        dream_add_game(dream_cube_directx12_check SOURCES src/cube_game.cpp
            CONFIG tests/directx12.toml SHADERS shaders/cube.slang ASSETS tests/reference)
        dream_add_game(dream_cube_opengl_check SOURCES src/cube_game.cpp
            CONFIG tests/opengl.toml SHADERS shaders/cube.slang ASSETS tests/reference)
        dream_add_game(dream_cube_opengles_check SOURCES src/cube_game.cpp
            CONFIG tests/opengles.toml SHADERS shaders/cube.slang ASSETS tests/reference)
    endif()

    # The whole path with no display or GPU: compiled shaders load, and every frame is recorded.
    dream_add_game(dream_cube_null SOURCES src/cube_game.cpp CONFIG tests/null.toml
        SHADERS shaders/cube.slang)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_cube_null COMMAND dream_cube_null)
        set_tests_properties(dream_cube_null PROPERTIES
            PASS_REGULAR_EXPRESSION "Using module 'null' for interface 'rhi'.*frames=5"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()

    # The DirectX check on WARP, Windows' software rasterizer, compares its picture with the
    # reference image, so build servers without a GPU check it too. See docs/adr/0021.
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        dream_add_game(dream_cube_warp_check SOURCES src/cube_game.cpp CONFIG tests/warp.toml
            SHADERS shaders/cube.slang ASSETS tests/reference)
        add_test(NAME dream_cube_warp_check COMMAND dream_cube_warp_check)
        set_tests_properties(dream_cube_warp_check PROPERTIES
            PASS_REGULAR_EXPRESSION "Screenshot matches")
        dream_add_game(dream_cube_directx12_warp_check SOURCES src/cube_game.cpp
            CONFIG tests/warp_directx12.toml SHADERS shaders/cube.slang ASSETS tests/reference)
        add_test(NAME dream_cube_directx12_warp_check COMMAND dream_cube_directx12_warp_check)
        set_tests_properties(dream_cube_directx12_warp_check PROPERTIES
            PASS_REGULAR_EXPRESSION "Screenshot matches")
    endif()
endif()
