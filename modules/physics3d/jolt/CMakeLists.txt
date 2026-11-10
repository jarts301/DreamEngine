# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(Jolt CONFIG REQUIRED GLOBAL)

dream_add_module(physics3d jolt SOURCES src/jolt_physics.cpp LIBRARIES Jolt::Jolt)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_physics3d_jolt_tests
        SOURCES
            tests/jolt_physics_test.cpp
        LIBRARIES
            dream::module_physics3d_jolt
    )
    target_include_directories(dream_physics3d_jolt_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
