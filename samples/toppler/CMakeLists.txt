dream_add_game(dream_toppler
    SOURCES src/toppler.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    RENDERER
    APPLICATION_ID org.dreamengine.toppler
    DISPLAY_NAME "Toppler"
)

if(DREAM_BUILD_TESTS)
    # The scripted run plays every level in turn, with no display, GPU or sound hardware, and checks
    # what it saved by reading it back.
    set(toppler_result "cleared=10 stars=[1-3][0-9] saved_stars=[1-3]+ saved_music=0.30 menu_opened=1")

    dream_add_game(dream_toppler_check SOURCES src/toppler.cpp CONFIG tests/check.toml
        ASSETS assets ../assets RENDERER)

    # The scripted run on the real GPU and sound hardware. Not a CTest, because build servers have
    # neither.
    dream_add_game(dream_toppler_vulkan_check SOURCES src/toppler.cpp CONFIG tests/vulkan.toml
        ASSETS assets ../assets tests/reference RENDERER)

    if(WIN32)
        dream_add_game(dream_toppler_directx_check SOURCES src/toppler.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets tests/reference RENDERER)

        # The check run with the other physics, audio and interface backends.
        dream_add_game(dream_toppler_alternatives_check SOURCES src/toppler.cpp
            CONFIG tests/alternatives.toml ASSETS assets ../assets RENDERER)
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_toppler_check COMMAND dream_toppler_check)
        set_tests_properties(dream_toppler_check PROPERTIES
            PASS_REGULAR_EXPRESSION "${toppler_result}"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")

        if(WIN32)
            add_test(NAME dream_toppler_alternatives_check COMMAND dream_toppler_alternatives_check)
            set_tests_properties(dream_toppler_alternatives_check PROPERTIES
                PASS_REGULAR_EXPRESSION "${toppler_result}"
                ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
        endif()
    endif()

    # The DirectX check on WARP, Windows' software rasterizer, compares its picture with the
    # reference image, so build servers without a GPU check it too. See docs/adr/0021.
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        dream_add_game(dream_toppler_warp_check SOURCES src/toppler.cpp CONFIG tests/warp.toml
            ASSETS assets ../assets tests/reference RENDERER)
        add_test(NAME dream_toppler_warp_check COMMAND dream_toppler_warp_check)
        set_tests_properties(dream_toppler_warp_check PROPERTIES
            PASS_REGULAR_EXPRESSION "Screenshot matches")
    endif()
endif()
