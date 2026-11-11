dream_add_game(dream_render_demo
    SOURCES src/render_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    RENDERER
    APPLICATION_ID org.dreamengine.render
    DISPLAY_NAME "Dream Renderer"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself. Not a CTest, because build servers have no GPU.
    dream_add_game(dream_render_demo_vulkan_check SOURCES src/render_demo.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets RENDERER)

    # The same view on DirectX, which only Windows can build.
    if(WIN32)
        dream_add_game(dream_render_demo_directx_check SOURCES src/render_demo.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets RENDERER)
    endif()

    # The whole path with no display or GPU: models load, and every part is drawn.
    dream_add_game(dream_render_demo_null SOURCES src/render_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets RENDERER)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_render_demo_null COMMAND dream_render_demo_null)
        set_tests_properties(dream_render_demo_null PROPERTIES
            PASS_REGULAR_EXPRESSION "frames=5 draws=6"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
