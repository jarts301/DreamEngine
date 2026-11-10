# GLOBAL because the game's final link needs this target, and it happens in another directory.
find_package(RmlUi CONFIG REQUIRED GLOBAL)

dream_add_module(ui rmlui SOURCES src/rmlui_game_ui.cpp LIBRARIES RmlUi::Core)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_ui_rmlui_tests
        SOURCES
            tests/rmlui_test.cpp
        LIBRARIES
            dream::module_ui_rmlui
    )
    target_include_directories(dream_ui_rmlui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    # The tests load a real font, read from the source tree.
    target_compile_definitions(dream_ui_rmlui_tests PRIVATE
        DREAM_TEST_FONT="${DreamEngine_SOURCE_DIR}/samples/assets/fonts/LatoLatin-Regular.ttf")
endif()
