dream_add_game(dream_render_demo
    SOURCES src/render_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
)

if(DREAM_BUILD_TESTS)
    dream_add_game(dream_render_demo_null SOURCES src/render_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_render_demo_null "frames=5 draws=12")

    # Compares a frame of the software renderer with the reference image. See docs/adr/0021.
    dream_add_game(dream_render_demo_screenshot SOURCES src/render_demo.cpp
        CONFIG tests/screenshot.toml ASSETS assets ../assets tests/reference)
    dream_add_sample_test(dream_render_demo_screenshot "Screenshot matches")
endif()
