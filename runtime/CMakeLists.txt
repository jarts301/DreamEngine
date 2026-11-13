# GLOBAL because every game's build needs EnTT's headers, and those are in other directories.
find_package(EnTT CONFIG REQUIRED GLOBAL)
find_package(Stb REQUIRED)
find_path(DREAM_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)

add_library(dream_runtime STATIC
    src/app.cpp
    src/asset_manager.cpp
    src/audio.cpp
    src/crash_reports.cpp
    src/debug_lines.cpp
    src/event_bus.cpp
    src/input_actions.cpp
    src/memory_budget.cpp
    src/memory_guard.cpp
    src/model_loader.cpp
    src/physics_2d.cpp
    src/physics_3d.cpp
    src/profiling.cpp
    src/program.cpp
    src/renderer.cpp
    src/resource_monitor.cpp
    src/scene_loader.cpp
    src/ui.cpp
    src/virtual_file_system.cpp
    src/world.cpp
)
add_library(dream::runtime ALIAS dream_runtime)

target_include_directories(dream_runtime PUBLIC include)
target_include_directories(dream_runtime SYSTEM PRIVATE
    ${Stb_INCLUDE_DIR} ${DREAM_CGLTF_INCLUDE_DIR})
target_link_libraries(dream_runtime PUBLIC
    dream::core dream::interface_platform dream::interface_rhi dream::interface_physics2d
    dream::interface_physics3d dream::interface_audio dream::interface_ui
    dream::interface_debug_ui dream::interface_profiler EnTT::EnTT)
dream_configure_target(dream_runtime)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_runtime_tests
        SOURCES
            tests/test_main.cpp
            tests/app_test.cpp
            tests/asset_manager_test.cpp
            tests/audio_test.cpp
            tests/crash_reports_test.cpp
            tests/event_bus_test.cpp
            tests/heap_counting_test.cpp
            tests/input_actions_test.cpp
            tests/memory_budget_test.cpp
            tests/model_loader_test.cpp
            tests/physics_test.cpp
            tests/profiling_test.cpp
            tests/renderer_test.cpp
            tests/scene_loader_test.cpp
            tests/ui_test.cpp
            tests/virtual_file_system_test.cpp
            tests/world_test.cpp
            # Profiled games count their allocations with this, so the tests run on top of it too.
            entry/heap_counting.cpp
        LIBRARIES
            dream::runtime
    )

    # Runs a game out of memory under a limit the program sets on itself, which can't be lifted,
    # so it is a program of its own.
    dream_add_test(dream_runtime_memory_exhaustion_test
        SOURCES
            tests/memory_exhaustion_test.cpp
        LIBRARIES
            dream::runtime
    )
endif()
