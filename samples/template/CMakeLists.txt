# The project template that `dream new` copies (ADR 0045), built here from templates/game as it is,
# placeholders and all, so a change to the engine that breaks new games fails the engine's own
# build and tests. The placeholders are only in strings and comments, which compile as they are.
set(template "${DreamEngine_SOURCE_DIR}/templates/game")

dream_add_game(dream_template
    SOURCES "${template}/src/__GAME_NAME__.cpp"
    CONFIG "${template}/dream.toml"
    ASSETS "${template}/assets"
)

if(DREAM_BUILD_TESTS)
    # Loads the template's scene and spins its crate for a few frames, with no display or GPU.
    dream_add_game(dream_template_null SOURCES "${template}/src/__GAME_NAME__.cpp"
        CONFIG tests/null.toml ASSETS "${template}/assets")

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dream_template_null COMMAND dream_template_null)
        set_tests_properties(dream_template_null PROPERTIES
            PASS_REGULAR_EXPRESSION "__GAME_TITLE__ ran 10 frames"
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endif()
