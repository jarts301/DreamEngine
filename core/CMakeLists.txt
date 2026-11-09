add_library(dream_core STATIC
    src/version.cpp
)
add_library(dream::core ALIAS dream_core)

target_include_directories(dream_core PUBLIC include)
target_compile_definitions(dream_core PRIVATE
    DREAM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DREAM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DREAM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
dream_configure_target(dream_core)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_core_tests
        SOURCES
            tests/test_main.cpp
            tests/version_test.cpp
            tests/cpp20_baseline_test.cpp
        LIBRARIES
            dream::core
    )
    target_compile_definitions(dream_core_tests PRIVATE
        DREAM_TEST_EXPECTED_VERSION="${PROJECT_VERSION}"
    )
endif()
