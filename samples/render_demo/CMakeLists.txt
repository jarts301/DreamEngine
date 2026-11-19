dream_add_game(dream_render_demo
    SOURCES src/render_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.render
    DISPLAY_NAME "Dream Renderer"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself. Not a CTest, because build servers have no GPU.
    dream_add_game(dream_render_demo_vulkan_check SOURCES src/render_demo.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets tests/reference)

    # The same view on DirectX, which only Windows can build.
    if(WIN32)
        dream_add_game(dream_render_demo_directx_check SOURCES src/render_demo.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_render_demo_directx12_check SOURCES src/render_demo.cpp
            CONFIG tests/directx12.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_render_demo_opengl_check SOURCES src/render_demo.cpp
            CONFIG tests/opengl.toml ASSETS assets ../assets tests/reference)
        dream_add_game(dream_render_demo_opengles_check SOURCES src/render_demo.cpp
            CONFIG tests/opengles.toml ASSETS assets ../assets tests/reference)
    endif()

    # The whole path with no display or GPU: models load, and every part is drawn.
    dream_add_game(dream_render_demo_null SOURCES src/render_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_render_demo_null COMMAND dream_render_demo_null)
        set_tests_properties(dream_render_demo_null PROPERTIES
            PASS_REGULAR_EXPRESSION "frames=5 draws=8"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()

    # The DirectX check on WARP, Windows' software rasterizer, compares its picture with the
    # reference image, so build servers without a GPU check it too. See docs/adr/0021.
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        dream_add_game(dream_render_demo_warp_check SOURCES src/render_demo.cpp CONFIG tests/warp.toml
            ASSETS assets ../assets tests/reference)
        add_test(NAME dream_render_demo_warp_check COMMAND dream_render_demo_warp_check)
        set_tests_properties(dream_render_demo_warp_check PROPERTIES
            PASS_REGULAR_EXPRESSION "Screenshot matches")
        dream_add_game(dream_render_demo_directx12_warp_check SOURCES src/render_demo.cpp
            CONFIG tests/warp_directx12.toml ASSETS assets ../assets tests/reference)
        add_test(NAME dream_render_demo_directx12_warp_check
            COMMAND dream_render_demo_directx12_warp_check)
        set_tests_properties(dream_render_demo_directx12_warp_check PROPERTIES
            PASS_REGULAR_EXPRESSION "Screenshot matches")
    endif()
endif()
