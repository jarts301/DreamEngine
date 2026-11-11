# Built on the engine's own physics library, libraries/impulse.
dream_add_module(physics3d impulse
    SOURCES src/impulse_physics.cpp
    LIBRARIES dream::library_impulse
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_physics3d_impulse_tests
        SOURCES
            tests/impulse_physics_test.cpp
        LIBRARIES
            dream::module_physics3d_impulse
    )
    target_include_directories(dream_physics3d_impulse_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
