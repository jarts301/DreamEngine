dream_add_game(dream_physics_demo
    SOURCES src/physics_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
    RENDERER
    APPLICATION_ID org.dreamengine.physics
    DISPLAY_NAME "Dream Physics"
)

if(DREAM_BUILD_TESTS)
    # Jolt and Box2D with no display or GPU: the scripted run checks what the simulation did.
    dream_add_game(dream_physics_demo_check SOURCES src/physics_demo.cpp CONFIG tests/physics.toml
        ASSETS assets ../assets RENDERER)

    # The same script with the null backends, which have no collisions: everything still loads,
    # steps and draws.
    dream_add_game(dream_physics_demo_null SOURCES src/physics_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets RENDERER)

    # The scripted run on the real GPU, with validation. Not a CTest, because build servers have no
    # GPU.
    dream_add_game(dream_physics_demo_vulkan_check SOURCES src/physics_demo.cpp
        CONFIG tests/vulkan.toml ASSETS assets ../assets RENDERER)

    # The same run on DirectX, which only Windows can build.
    if(WIN32)
        dream_add_game(dream_physics_demo_directx_check SOURCES src/physics_demo.cpp
            CONFIG tests/directx.toml ASSETS assets ../assets RENDERER)
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_physics_demo_check COMMAND dream_physics_demo_check)
        set_tests_properties(dream_physics_demo_check PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bodies3d=20 bodies2d=9 pyramid=standing pendulum=held zone=[1-9][0-9]* stack2d=standing seesaw=tilted contacts2d=[1-9][0-9]* lines=drawn"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")

        add_test(NAME dream_physics_demo_null COMMAND dream_physics_demo_null)
        set_tests_properties(dream_physics_demo_null PROPERTIES
            PASS_REGULAR_EXPRESSION "bodies3d=20 bodies2d=9 .* lines=drawn"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
