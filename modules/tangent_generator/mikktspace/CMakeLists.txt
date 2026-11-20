# Built from the sources in third_party/ (ADR 0039).
dream_add_module(tangent_generator mikktspace
    SOURCES src/mikktspace_tangent_generator.cpp
    LIBRARIES mikktspace::mikktspace
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_tangent_generator_mikktspace_tests
        SOURCES
            tests/test_main.cpp
            tests/mikktspace_tangent_generator_test.cpp
        LIBRARIES
            dream::module_tangent_generator_mikktspace
    )
endif()
