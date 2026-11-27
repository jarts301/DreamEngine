dream_add_game(dream_top_down
    SOURCES src/top_down.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.topdown
    DISPLAY_NAME "Dream Top-Down"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself. Not a CTest, because build servers have no GPU.
    dream_add_game(dream_top_down_vulkan_check SOURCES src/top_down.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets tests/reference)

    if(WIN32)
        dream_add_game(dream_top_down_directx_check SOURCES src/top_down.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_top_down_directx12_check SOURCES src/top_down.cpp
            CONFIG tests/directx12.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_top_down_opengl_check SOURCES src/top_down.cpp
            CONFIG tests/opengl.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_top_down_opengles_check SOURCES src/top_down.cpp
            CONFIG tests/opengles.toml ASSETS assets ../assets tests/reference)
    endif()

    # The garden loads from its Tiled map, and its tiles and sprites are drawn, with no display or GPU.
    dream_add_game(dream_top_down_null SOURCES src/top_down.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)

    # A scripted walk with real 2D physics: along the path, and up into the stone wall, which stops it.
    dream_add_game(dream_top_down_check SOURCES src/top_down.cpp CONFIG tests/check.toml
        ASSETS assets ../assets)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_top_down_null COMMAND dream_top_down_null)
        set_tests_properties(dream_top_down_null PROPERTIES
            PASS_REGULAR_EXPRESSION "frames=5 sprites=[1-9][0-9]+"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
        add_test(NAME dream_top_down_check COMMAND dream_top_down_check)
        set_tests_properties(dream_top_down_check PROPERTIES
            PASS_REGULAR_EXPRESSION "blocked=1"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
