dream_add_module(profiler null SOURCES src/null_profiler.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_profiler_null_tests
        SOURCES
            tests/null_profiler_test.cpp
        LIBRARIES
            dream::module_profiler_null
    )
    target_include_directories(dream_profiler_null_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
