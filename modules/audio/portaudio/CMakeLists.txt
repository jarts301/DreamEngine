# PortAudio (MIT) plays the output, dr_libs (MIT-0 or public domain) decode WAV, MP3 and FLAC, and
# the engine's own mixer library mixes. See docs/adr/0014-more-backends-and-libraries.md.
find_path(DREAM_DRLIBS_INCLUDE_DIR dr_wav.h REQUIRED)

# PortAudio's CMake package has had several names and targets across versions; without one, as on
# a Linux system install, the header and library are found directly.
find_package(PortAudio CONFIG QUIET GLOBAL)
if(NOT PortAudio_FOUND)
    find_package(portaudio CONFIG QUIET GLOBAL)
endif()
if(TARGET PortAudio::PortAudio)
    set(portaudio_libraries PortAudio::PortAudio)
elseif(TARGET portaudio_static)
    set(portaudio_libraries portaudio_static)
elseif(TARGET portaudio)
    set(portaudio_libraries portaudio)
else()
    find_path(DREAM_PORTAUDIO_INCLUDE_DIR portaudio.h REQUIRED)
    find_library(DREAM_PORTAUDIO_LIBRARY NAMES portaudio portaudio_static REQUIRED)
    set(portaudio_libraries ${DREAM_PORTAUDIO_LIBRARY})
    if(WIN32)
        list(APPEND portaudio_libraries winmm ole32 uuid setupapi)
    endif()
endif()

dream_add_module(audio portaudio
    SOURCES src/portaudio_audio.cpp src/decoder.cpp
    LIBRARIES dream::library_mixer ${portaudio_libraries}
)
target_include_directories(dream_module_audio_portaudio SYSTEM PRIVATE ${DREAM_DRLIBS_INCLUDE_DIR})
if(DREAM_PORTAUDIO_INCLUDE_DIR)
    target_include_directories(dream_module_audio_portaudio SYSTEM PRIVATE
        ${DREAM_PORTAUDIO_INCLUDE_DIR})
endif()

# decoder.cpp compiles dr_libs' implementation, whose code isn't held to the engine's warnings.
if(MSVC)
    set_source_files_properties(src/decoder.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/decoder.cpp PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(dream_module_audio_portaudio PRIVATE m)
endif()

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_audio_portaudio_tests
        SOURCES
            tests/portaudio_test.cpp
        LIBRARIES
            dream::module_audio_portaudio
    )
    target_include_directories(dream_audio_portaudio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
