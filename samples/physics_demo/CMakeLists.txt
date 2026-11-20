dream_add_game(dream_physics_demo
    SOURCES src/physics_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
)

if(DREAM_BUILD_TESTS)
    # The scripted run with impulse and Box2D, drawn by the software renderer.
    dream_add_game(dream_physics_demo_check SOURCES src/physics_demo.cpp CONFIG tests/physics.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_physics_demo_check
        "bodies3d=20 bodies2d=9 pyramid=standing pendulum=held zone=[1-9][0-9]* stack2d=standing seesaw=tilted contacts2d=[1-9][0-9]* lines=drawn")

    dream_add_game(dream_physics_demo_null SOURCES src/physics_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_physics_demo_null "bodies3d=20 bodies2d=9 .* lines=drawn")
endif()
