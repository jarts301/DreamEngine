dream_add_module(profiler timeline
    SOURCES src/timeline_profiler.cpp
    LIBRARIES dream::library_timeline
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_profiler_timeline_tests
        SOURCES
            tests/timeline_profiler_test.cpp
        LIBRARIES
            dream::module_profiler_timeline
    )
    target_include_directories(dream_profiler_timeline_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
