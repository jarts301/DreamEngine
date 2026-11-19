# The renderer's shaders come with it: a game that lists 'forward' gets them compiled for each of its
# graphics backends' formats.
dream_add_module(renderer forward
    SOURCES src/forward_renderer.cpp
    SHADERS
        shaders/forward.slang
        shaders/forward_masked.slang
        shaders/shadow.slang
        shaders/shadow_masked.slang
        shaders/tonemap.slang
        shaders/debug_lines.slang
        shaders/ui.slang
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_renderer_forward_tests
        SOURCES
            tests/test_main.cpp
            tests/forward_renderer_test.cpp
        LIBRARIES
            dream::module_renderer_forward
    )
endif()
