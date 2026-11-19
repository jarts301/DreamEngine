dream_add_module(renderer null SOURCES src/null_renderer.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_renderer_null_tests
        SOURCES
            tests/test_main.cpp
            tests/null_renderer_test.cpp
        LIBRARIES
            dream::module_renderer_null
    )
endif()
