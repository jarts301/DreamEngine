# GLOBAL because the game's final link needs these targets, and it happens in another directory.
find_package(ozz-animation CONFIG REQUIRED GLOBAL)

dream_add_module(animation ozz
    SOURCES src/ozz_animation_system.cpp
    LIBRARIES ozz::animation_offline ozz::animation ozz::base
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_animation_ozz_tests
        SOURCES
            tests/test_main.cpp
            tests/ozz_animation_system_test.cpp
        LIBRARIES
            dream::module_animation_ozz
    )
endif()
