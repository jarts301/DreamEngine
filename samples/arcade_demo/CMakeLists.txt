dream_add_game(dream_arcade_demo
    SOURCES src/arcade_demo.cpp
    CONFIG dream.toml
    ASSETS assets ../assets
)

if(DREAM_BUILD_TESTS)
    # The scripted run throws balls, pauses, changes the music volume and resumes, drawn by the
    # software renderer with every interface.
    dream_add_game(dream_arcade_demo_check SOURCES src/arcade_demo.cpp CONFIG tests/check.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_arcade_demo_check
        "score=[1-9] hits_heard=[1-9][0-9]* menu_opened=1 music_volume=0.25 debug_drawn=yes ui_commands=some")

    # The same with every null backend.
    dream_add_game(dream_arcade_demo_null SOURCES src/arcade_demo.cpp CONFIG tests/null.toml
        ASSETS assets ../assets)
    dream_add_sample_test(dream_arcade_demo_null
        "menu_opened=1 music_volume=0.25 debug_drawn=yes ui_commands=none")

    # Compares a frame of the software renderer with the reference image. See docs/adr/0021.
    dream_add_game(dream_arcade_demo_screenshot SOURCES src/arcade_demo.cpp
        CONFIG tests/screenshot.toml ASSETS assets ../assets tests/reference)
    dream_add_sample_test(dream_arcade_demo_screenshot "Screenshot matches")
endif()
