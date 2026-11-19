# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(meshoptimizer CONFIG REQUIRED GLOBAL)

dream_add_module(mesh_simplifier meshoptimizer
    SOURCES src/meshoptimizer_simplifier.cpp
    LIBRARIES meshoptimizer::meshoptimizer
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_mesh_simplifier_meshoptimizer_tests
        SOURCES
            tests/test_main.cpp
            tests/meshoptimizer_simplifier_test.cpp
        LIBRARIES
            dream::module_mesh_simplifier_meshoptimizer
    )
endif()
