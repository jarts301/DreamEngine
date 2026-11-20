dream_add_game(dream_toppler
    SOURCES src/toppler.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
)

if(DREAM_BUILD_TESTS)
    # Plays every level, drawn by the software renderer.
    dream_add_game(dream_toppler_check SOURCES src/toppler.cpp CONFIG tests/check.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_toppler_check
        "cleared=10 stars=[1-3][0-9] saved_stars=[1-3]+ saved_music=0.30 menu_opened=1")
    # Its saved progress goes to a folder of its own, not the player's.
    set_tests_properties(dream_toppler_check PROPERTIES
        ENVIRONMENT "DREAM_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/dream_toppler_check_user")

    # Compares a frame of the software renderer with the reference image. See docs/adr/0021.
    dream_add_game(dream_toppler_screenshot SOURCES src/toppler.cpp CONFIG tests/screenshot.toml
        ASSETS assets ../assets tests/reference)
    dream_add_sample_test(dream_toppler_screenshot "Screenshot matches")
    set_tests_properties(dream_toppler_screenshot PROPERTIES
        ENVIRONMENT "DREAM_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/dream_toppler_screenshot_user")
endif()
