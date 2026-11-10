dream_add_module(physics3d null SOURCES src/null_physics.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_physics3d_null_tests
        SOURCES
            tests/null_physics_test.cpp
        LIBRARIES
            dream::module_physics3d_null
    )
    target_include_directories(dream_physics3d_null_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
