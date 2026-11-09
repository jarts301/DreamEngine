dream_add_game(dream_scene_demo
    SOURCES src/scene_demo.cpp
    CONFIG dream.toml
    ASSETS assets
    SHADERS shaders/solid.slang
    APPLICATION_ID org.dreamengine.scenes
    DISPLAY_NAME "Dream Scenes"
)

if(DREAM_BUILD_TESTS)
    # Loads both scenes and swaps between them, with no display or GPU needed.
    dream_add_game(dream_scene_demo_null SOURCES src/scene_demo.cpp CONFIG tests/null.toml
        ASSETS assets SHADERS shaders/solid.slang)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_scene_demo_null COMMAND dream_scene_demo_null)
        set_tests_properties(dream_scene_demo_null PROPERTIES
            PASS_REGULAR_EXPRESSION "frames=30 scene=assets:/scenes/orbit.toml switches=2 pipeline_loads=1"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
