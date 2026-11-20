dream_add_game(dream_hello_game
    SOURCES src/hello_game.cpp
    CONFIG dream.toml
    ASSETS assets
)

# The same game run to completion without a person, on each platform backend.
function(dream_add_hello_test name config expected_output)
    dream_add_game(${name} SOURCES src/hello_game.cpp CONFIG ${config} ASSETS assets)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected_output}")
endfunction()

if(DREAM_BUILD_TESTS)
    dream_add_hello_test(dream_hello_game_null tests/null.toml
        "Using module 'null'.*frames=3 asset=<missing> runs=1")
    # The web platform with its window hidden reads real assets and keeps real user files.
    dream_add_hello_test(dream_hello_game_web tests/web.toml
        "Using module 'web'.*frames=3 asset=Hello from the assets folder runs=[0-9]+")
    set_tests_properties(dream_hello_game_web PROPERTIES
        ENVIRONMENT "DREAM_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/dream_hello_game_web_user")

    # The game with the timeline profiler. The check runs it, then reads the trace it saved for the
    # engine's zones, the game's own zone and value, the engine's frame, memory and heap values, and
    # one frame mark per frame.
    dream_add_game(dream_hello_game_profiler SOURCES src/hello_game.cpp
        CONFIG tests/profiler.toml ASSETS assets)
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

    # The game crashes on purpose, as [crash_report] simulate asks, and the check reads the report
    # it leaves: the game, the frame, the modules, and the log up to the crash.
    dream_add_game(dream_hello_game_crash SOURCES src/hello_game.cpp CONFIG tests/crash.toml
        ASSETS assets)
    set(expected_report
        "Application: Dream Hello" "Frame: 2" "platform module: null"
        "Segmentation fault (SIGSEGV)" "Crashing on purpose, with 'null_pointer'" "Stack trace")
    list(JOIN expected_report "|" expected_report)
    set(report_folder "${CMAKE_CURRENT_BINARY_DIR}/dream_hello_game_crash_reports")
    add_test(NAME dream_hello_game_crash
        COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:dream_hello_game_crash>"
            "-DDIRECTORY=${report_folder}"
            "-DEXPECT=${expected_report}"
            "-DMINIDUMP=OFF"
            -P "${DreamEngine_SOURCE_DIR}/cmake/tests/check_crash_report.cmake"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
