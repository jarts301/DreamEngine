dream_add_library(mixer SOURCES src/mixer.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_library_mixer_tests
        SOURCES
            tests/mixer_test.cpp
        LIBRARIES
            dream::library_mixer
    )
endif()
