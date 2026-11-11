# Built on the engine's own interface library, libraries/markup, which reads the same RML and RCSS
# files as RmlUi.
dream_add_module(ui markup
    SOURCES src/markup_game_ui.cpp
    LIBRARIES dream::library_markup
)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_ui_markup_tests
        SOURCES
            tests/markup_ui_test.cpp
        LIBRARIES
            dream::module_ui_markup
    )
    target_include_directories(dream_ui_markup_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    # The tests load a real font, read from the source tree.
    target_compile_definitions(dream_ui_markup_tests PRIVATE
        DREAM_TEST_FONT="${DreamEngine_SOURCE_DIR}/samples/assets/fonts/LatoLatin-Regular.ttf")
endif()
