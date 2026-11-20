# Built from the sources in third_party/ (ADR 0039).
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
