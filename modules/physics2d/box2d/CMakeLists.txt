# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(box2d CONFIG REQUIRED GLOBAL)

dream_add_module(physics2d box2d SOURCES src/box2d_physics.cpp LIBRARIES box2d::box2d)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_physics2d_box2d_tests
        SOURCES
            tests/box2d_physics_test.cpp
        LIBRARIES
            dream::module_physics2d_box2d
    )
    target_include_directories(dream_physics2d_box2d_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
