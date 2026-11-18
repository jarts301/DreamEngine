set(render_targets_shaders shaders/shadow.slang shaders/scene.slang shaders/composite.slang)

dream_add_game(dream_render_targets
    SOURCES src/render_targets.cpp
    CONFIG dream.toml
    SHADERS ${render_targets_shaders}
    APPLICATION_ID org.dreamengine.targets
    DISPLAY_NAME "Dream Render Targets"
)

if(DREAM_BUILD_TESTS)
    # A fixed view on the real GPU that ends by itself, compared with one reference image on every
    # graphics backend (ADR 0027). Not CTests, because build servers have no GPU.
    set(render_targets_backends vulkan)
    if(WIN32)
        list(APPEND render_targets_backends directx directx12 opengl opengles)
    endif()
    foreach(backend IN LISTS render_targets_backends)
        dream_add_game(dream_render_targets_${backend}_check SOURCES src/render_targets.cpp
            CONFIG tests/${backend}.toml SHADERS ${render_targets_shaders} ASSETS tests/reference)
    endforeach()

    # The whole path with no display or GPU.
    dream_add_game(dream_render_targets_null SOURCES src/render_targets.cpp CONFIG tests/null.toml
        SHADERS ${render_targets_shaders})
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_render_targets_null COMMAND dream_render_targets_null)
        set_tests_properties(dream_render_targets_null PROPERTIES
            PASS_REGULAR_EXPRESSION "Using module 'null' for interface 'rhi'.*frames=5"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()

    # The Direct3D checks on WARP, Windows' software rasterizer, which build servers run too.
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        foreach(check IN ITEMS warp warp_directx12)
            dream_add_game(dream_render_targets_${check}_check SOURCES src/render_targets.cpp
                CONFIG tests/${check}.toml SHADERS ${render_targets_shaders}
                ASSETS tests/reference)
            add_test(NAME dream_render_targets_${check}_check
                COMMAND dream_render_targets_${check}_check)
            set_tests_properties(dream_render_targets_${check}_check PROPERTIES
                PASS_REGULAR_EXPRESSION "Screenshot matches")
        endforeach()
    endif()
endif()
