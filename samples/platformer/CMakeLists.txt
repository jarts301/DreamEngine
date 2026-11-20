dream_add_game(dream_platformer
    SOURCES src/platformer.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.platformer
    DISPLAY_NAME "Dream Platformer"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself. Not a CTest, because build servers have no GPU.
    dream_add_game(dream_platformer_vulkan_check SOURCES src/platformer.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets tests/reference)

    if(WIN32)
        dream_add_game(dream_platformer_directx_check SOURCES src/platformer.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_platformer_directx12_check SOURCES src/platformer.cpp
            CONFIG tests/directx12.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_platformer_opengl_check SOURCES src/platformer.cpp
            CONFIG tests/opengl.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_platformer_opengles_check SOURCES src/platformer.cpp
            CONFIG tests/opengles.toml ASSETS assets ../assets tests/reference)
    endif()

    # The level loads and its tiles and sprites are drawn, with no display or GPU.
    dream_add_game(dream_platformer_null SOURCES src/platformer.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)

    # A scripted run with real 2D physics: right along the ground, over a pit, collecting coins.
    dream_add_game(dream_platformer_check SOURCES src/platformer.cpp CONFIG tests/check.toml
        ASSETS assets ../assets)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_platformer_null COMMAND dream_platformer_null)
        set_tests_properties(dream_platformer_null PROPERTIES
            PASS_REGULAR_EXPRESSION "frames=5 sprites=[1-9][0-9]+"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
        add_test(NAME dream_platformer_check COMMAND dream_platformer_check)
        set_tests_properties(dream_platformer_check PROPERTIES
            PASS_REGULAR_EXPRESSION "coins=[1-9]"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
