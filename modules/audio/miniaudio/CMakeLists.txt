find_path(DREAM_MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

dream_add_module(audio miniaudio
    SOURCES src/miniaudio_audio.cpp src/miniaudio_impl.cpp
    LIBRARIES ${CMAKE_DL_LIBS}
)
target_include_directories(dream_module_audio_miniaudio SYSTEM PRIVATE ${DREAM_MINIAUDIO_INCLUDE_DIR})

# Both files must agree on these. Encoding and file loading are done by the engine, not miniaudio.
target_compile_definitions(dream_module_audio_miniaudio PRIVATE MA_NO_ENCODING MA_NO_GENERATION)

if(MSVC)
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
    find_package(Threads REQUIRED)
    target_link_libraries(dream_module_audio_miniaudio PRIVATE Threads::Threads m)
endif()

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_audio_miniaudio_tests
        SOURCES
            tests/miniaudio_test.cpp
        LIBRARIES
            dream::module_audio_miniaudio
    )
    target_include_directories(dream_audio_miniaudio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
