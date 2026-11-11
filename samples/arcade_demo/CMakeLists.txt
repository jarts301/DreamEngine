dream_add_game(dream_arcade_demo
    SOURCES src/arcade_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    RENDERER
    APPLICATION_ID org.dreamengine.arcade
    DISPLAY_NAME "Dream Arcade"
)

if(DREAM_BUILD_TESTS)
    # RmlUi, ImGui, miniaudio (mixing without sound hardware) and Jolt, with no display or GPU. The
    # scripted run throws balls, pauses, changes the music volume and resumes.
    dream_add_game(dream_arcade_demo_check SOURCES src/arcade_demo.cpp CONFIG tests/check.toml
        ASSETS assets ../assets RENDERER)

    # The same with every null backend.
    dream_add_game(dream_arcade_demo_null SOURCES src/arcade_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets RENDERER)

    # The scripted run on the real GPU and sound hardware, with validation. Not a CTest, because
    # build servers have neither.
    dream_add_game(dream_arcade_demo_vulkan_check SOURCES src/arcade_demo.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets RENDERER)

    # The same run on DirectX, which only Windows can build.
    if(WIN32)
        dream_add_game(dream_arcade_demo_directx_check SOURCES src/arcade_demo.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets RENDERER)
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_arcade_demo_check COMMAND dream_arcade_demo_check)
        set_tests_properties(dream_arcade_demo_check PROPERTIES
            PASS_REGULAR_EXPRESSION
                "score=[1-9] hits_heard=[1-9][0-9]* menu_opened=1 music_volume=0.25 debug_drawn=yes ui_commands=some"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")

        add_test(NAME dream_arcade_demo_null COMMAND dream_arcade_demo_null)
        set_tests_properties(dream_arcade_demo_null PROPERTIES
            PASS_REGULAR_EXPRESSION "menu_opened=1 music_volume=0.25 debug_drawn=yes ui_commands=none"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
