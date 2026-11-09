# GLOBAL because every game's build needs EnTT's headers, and those are in other directories.
find_package(EnTT CONFIG REQUIRED GLOBAL)

add_library(dream_runtime STATIC
    src/app.cpp
    src/asset_manager.cpp
    src/event_bus.cpp
    src/input_actions.cpp
    src/program.cpp
    src/scene_loader.cpp
    src/virtual_file_system.cpp
    src/world.cpp
)
add_library(dream::runtime ALIAS dream_runtime)

target_include_directories(dream_runtime PUBLIC include)
target_link_libraries(dream_runtime PUBLIC
    dream::core dream::interface_platform dream::interface_rhi EnTT::EnTT)
dream_configure_target(dream_runtime)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_runtime_tests
        SOURCES
            tests/test_main.cpp
            tests/app_test.cpp
            tests/asset_manager_test.cpp
            tests/event_bus_test.cpp
            tests/input_actions_test.cpp
            tests/scene_loader_test.cpp
            tests/virtual_file_system_test.cpp
            tests/world_test.cpp
        LIBRARIES
            dream::runtime
    )
endif()
