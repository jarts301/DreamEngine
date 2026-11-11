# stb_truetype rasterizes the glyphs. stb is MIT or public domain, and the runtime already uses it.
find_package(Stb REQUIRED)

dream_add_library(markup
    SOURCES
        src/context.cpp
        src/fonts.cpp
        src/layout.cpp
        src/parser.cpp
        src/render.cpp
        src/style.cpp
)
target_include_directories(dream_library_markup SYSTEM PRIVATE ${Stb_INCLUDE_DIR})

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_library_markup_tests
        SOURCES
            tests/markup_test.cpp
        LIBRARIES
            dream::library_markup
    )
    # The tests load a real font, read from the source tree.
    target_compile_definitions(dream_library_markup_tests PRIVATE
        DREAM_TEST_FONT="${DreamEngine_SOURCE_DIR}/samples/assets/fonts/LatoLatin-Regular.ttf"
        DREAM_TEST_BOLD_FONT="${DreamEngine_SOURCE_DIR}/samples/assets/fonts/LatoLatin-Bold.ttf"
        DREAM_TEST_SAMPLES="${DreamEngine_SOURCE_DIR}/samples")
endif()
