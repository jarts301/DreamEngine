find_package(Threads REQUIRED)

# Header-only use of toml++, private to src/config.cpp. See the comment in that file.
find_path(DREAM_TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

add_library(dream_core STATIC
    src/assert.cpp
    src/config.cpp
    src/job_system.cpp
    src/linear_allocator.cpp
    src/log.cpp
    src/memory.cpp
    src/module_project.cpp
    src/module_registry.cpp
    src/pool_allocator.cpp
    src/process_stats.cpp
    src/profile.cpp
    src/version.cpp
)
add_library(dream::core ALIAS dream_core)

target_include_directories(dream_core PUBLIC include)
target_include_directories(dream_core SYSTEM PRIVATE ${DREAM_TOMLPLUSPLUS_INCLUDE_DIR})
target_link_libraries(dream_core PUBLIC Threads::Threads)
if(ANDROID)
    target_link_libraries(dream_core PRIVATE log)
elseif(WIN32)
    # GetProcessMemoryInfo, for process_stats.cpp.
    target_link_libraries(dream_core PRIVATE psapi)
endif()
target_compile_definitions(dream_core PRIVATE
    DREAM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DREAM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DREAM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
dream_configure_target(dream_core)
if(NOT DREAM_PROFILING)
    target_compile_definitions(dream_core PUBLIC DREAM_DISABLE_PROFILING)
endif()

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_core_tests
        SOURCES
            tests/test_main.cpp
            tests/assert_test.cpp
            tests/clock_test.cpp
            tests/config_test.cpp
            tests/cpp20_baseline_test.cpp
            tests/handle_test.cpp
            tests/job_system_test.cpp
            tests/log_test.cpp
            tests/math_test.cpp
            tests/memory_test.cpp
            tests/module_test.cpp
            tests/process_stats_test.cpp
            tests/profile_disabled_test.cpp
            tests/profile_test.cpp
            tests/result_test.cpp
            tests/version_test.cpp
        LIBRARIES
            dream::core
    )
    target_compile_definitions(dream_core_tests PRIVATE
        DREAM_TEST_EXPECTED_VERSION="${PROJECT_VERSION}"
    )
endif()
