dream_add_module(audio null SOURCES src/null_audio.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_audio_null_tests
        SOURCES
            tests/null_audio_test.cpp
        LIBRARIES
            dream::module_audio_null
    )
    target_include_directories(dream_audio_null_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
