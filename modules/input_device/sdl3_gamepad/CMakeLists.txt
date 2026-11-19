# GLOBAL because the game's final link needs SDL's targets, and it happens in another directory.
# The platform module may have found it already; this module also works without it.
find_package(SDL3 CONFIG REQUIRED GLOBAL)

dream_add_module(input_device sdl3_gamepad
    SOURCES src/sdl3_gamepad.cpp
    LIBRARIES SDL3::SDL3
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_input_device_sdl3_gamepad_tests
        SOURCES
            tests/test_main.cpp
            tests/sdl3_gamepad_test.cpp
        LIBRARIES
            dream::module_input_device_sdl3_gamepad
            SDL3::SDL3
    )
endif()
