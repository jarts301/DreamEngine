dream_add_library(impulse SOURCES src/collide.cpp src/geometry.cpp src/world.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_library_impulse_tests
        SOURCES
            tests/impulse_test.cpp
        LIBRARIES
            dream::library_impulse
    )
endif()
