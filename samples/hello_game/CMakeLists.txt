dream_add_game(dream_hello_game
    SOURCES src/hello_game.cpp
    CONFIG dream.toml
    ASSETS assets
    APPLICATION_ID org.dreamengine.hello
    DISPLAY_NAME "Dream Hello"
)

# The same game run to completion without a person, on each platform backend.
function(dream_add_hello_test name config expected_output)
    dream_add_game(${name} SOURCES src/hello_game.cpp CONFIG ${config} ASSETS assets)

    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${expected_output}"
            # SDL's dummy driver needs no display, so this also passes on a build server.
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    endif()
endfunction()

if(DREAM_BUILD_TESTS)
    dream_add_hello_test(dream_hello_game_null tests/null.toml
        "Using module 'null'.*frames=3 asset=<missing> runs=1")
    dream_add_hello_test(dream_hello_game_sdl3 tests/sdl3.toml
        "Using module 'sdl3'.*frames=3 asset=Hello from the assets folder runs=[0-9]+")

    # The game with the timeline profiler. The check runs it, then reads the trace it saved for the
    # engine's zones, the game's own zone and value, and one frame mark per frame. Profiling
    # targets PC for now, so this is not built for Android.
    if(NOT ANDROID)
        dream_add_game(dream_hello_game_profiler SOURCES src/hello_game.cpp
            CONFIG tests/profiler.toml ASSETS assets)

        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME dream_hello_game_profiler
                COMMAND ${CMAKE_COMMAND}
                    "-DPROGRAM=$<TARGET_FILE:dream_hello_game_profiler>"
                    "-DTRACE=${CMAKE_CURRENT_BINARY_DIR}/dream_hello_game_profiler_trace.json"
                    "-DZONES=Start|Count runs|Platform events|Frame|Queued events|Update|Stop"
                    "-DVALUES=Hello frames"
                    "-DTHREADS=Main"
                    "-DFRAMES=3"
                    -P "${DreamEngine_SOURCE_DIR}/cmake/tests/check_trace.cmake"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()
endif()
