find_package(Threads REQUIRED)

# Header-only use of toml++, private to src/config.cpp. See the comment in that file.
find_path(DREAM_TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

add_library(dream_core STATIC
    src/assert.cpp
    src/config.cpp
    src/crash_handler_linux.cpp
    src/crash_handler_windows.cpp
    src/crash_report.cpp
    src/job_system.cpp
    src/linear_allocator.cpp
    src/log.cpp
    src/memory.cpp
    src/memory_reserve.cpp
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
    # GetProcessMemoryInfo, for process_stats.cpp, and stack walks and minidumps for crash reports.
    target_link_libraries(dream_core PRIVATE psapi dbghelp)
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
            tests/crash_handler_test.cpp
            tests/cpp20_baseline_test.cpp
            tests/handle_test.cpp
            tests/job_system_test.cpp
            tests/log_test.cpp
            tests/math_test.cpp
            tests/memory_reserve_test.cpp
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
    # The crash handler's internals are tested directly, because a crash can't happen in a test.
    target_include_directories(dream_core_tests PRIVATE src)

    # Real crashes happen in a program of their own, which crashes the way it is told, and a
    # script checks the report each one leaves. Crash reports are for PC for now.
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(dream_crash_probe tests/crash_probe.cpp)
        target_link_libraries(dream_crash_probe PRIVATE dream::core)
        dream_configure_target(dream_crash_probe)

        if(NOT CMAKE_CROSSCOMPILING)
            if(WIN32)
                set(reason_null_pointer "Access violation, writing address 0x0000000000000000")
                set(reason_stack_overflow "Stack overflow")
            else()
                set(reason_null_pointer "Segmentation fault (SIGSEGV)")
                set(reason_stack_overflow "most likely a stack overflow")
            endif()
            set(reason_divide_by_zero "division by zero")
            set(reason_abort "abort() was called")
            set(reason_assert "DREAM_VERIFY|A simulated assertion failure")
            set(reason_terminate "std::terminate was called")

            set(kinds null_pointer stack_overflow abort assert terminate)
            # Only x86 processors trap an integer division by zero.
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
                list(APPEND kinds divide_by_zero)
            endif()
            foreach(kind IN LISTS kinds)
                set(folder "${CMAKE_CURRENT_BINARY_DIR}/crash_reports_${kind}")
                set(expected "Application: Crash probe" "Frame: 42" "Probe: about to crash"
                    "The probe is about to crash with ${kind}" "Stack trace" "${reason_${kind}}")
                list(JOIN expected "|" expected)
                add_test(NAME dream_crash_report_${kind}
                    COMMAND ${CMAKE_COMMAND}
                        "-DPROGRAM=$<TARGET_FILE:dream_crash_probe>"
                        "-DARGUMENTS=${kind}|${folder}"
                        "-DDIRECTORY=${folder}"
                        "-DEXPECT=${expected}"
                        "-DMINIDUMP=${WIN32}"
                        -P "${PROJECT_SOURCE_DIR}/cmake/tests/check_crash_report.cmake")
            endforeach()
        endif()
    endif()
endif()
