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
    # engine's zones, the game's own zone and value, the engine's frame, memory and heap values, and
    # one frame mark per frame. Profiling targets PC for now, so this is not built for Android.
    if(NOT ANDROID)
        dream_add_game(dream_hello_game_profiler SOURCES src/hello_game.cpp
            CONFIG tests/profiler.toml ASSETS assets)

        if(NOT CMAKE_CROSSCOMPILING)
            set(expected_zones
                "Start" "Count runs" "Platform events" "Frame" "Queued events" "Update"
                "Profiler sampling" "Stop")
            list(JOIN expected_zones "|" expected_zones)
            set(expected_values
                "Hello frames" "Frame time (ms)" "Frame CPU work (ms)" "Fixed steps per frame"
                "Memory resident (MB)" "Memory private (MB)" "Heap allocations per frame"
                "Heap in use (MB)")
            list(JOIN expected_values "|" expected_values)
            add_test(NAME dream_hello_game_profiler
                COMMAND ${CMAKE_COMMAND}
                    "-DPROGRAM=$<TARGET_FILE:dream_hello_game_profiler>"
                    "-DTRACE=${CMAKE_CURRENT_BINARY_DIR}/dream_hello_game_profiler_trace.json"
                    "-DZONES=${expected_zones}"
                    "-DVALUES=${expected_values}"
                    "-DTHREADS=Main"
                    "-DFRAMES=3"
                    -P "${DreamEngine_SOURCE_DIR}/cmake/tests/check_trace.cmake"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()

        # The game crashes on purpose, as [crash_report] simulate asks, and the check reads the
        # report it leaves: the game, the frame, the modules, and the log up to the crash.
        dream_add_game(dream_hello_game_crash SOURCES src/hello_game.cpp CONFIG tests/crash.toml
            ASSETS assets)

        if(NOT CMAKE_CROSSCOMPILING)
            if(WIN32)
                set(crash_reason "Access violation, writing address 0x0000000000000000")
            else()
                set(crash_reason "Segmentation fault (SIGSEGV)")
            endif()
            set(expected_report
                "Application: Dream Hello" "Frame: 2" "platform module: null" "${crash_reason}"
                "Crashing on purpose, with 'null_pointer'" "Stack trace")
            list(JOIN expected_report "|" expected_report)
            set(report_folder "${CMAKE_CURRENT_BINARY_DIR}/dream_hello_game_crash_reports")
            add_test(NAME dream_hello_game_crash
                COMMAND ${CMAKE_COMMAND}
                    "-DPROGRAM=$<TARGET_FILE:dream_hello_game_crash>"
                    "-DDIRECTORY=${report_folder}"
                    "-DEXPECT=${expected_report}"
                    "-DMINIDUMP=${WIN32}"
                    -P "${DreamEngine_SOURCE_DIR}/cmake/tests/check_crash_report.cmake"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()
endif()
