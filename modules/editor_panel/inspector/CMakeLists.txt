dream_add_module(editor_panel inspector SOURCES src/inspector_panel.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_editor_panel_inspector_tests
        SOURCES
            tests/test_main.cpp
            tests/inspector_panel_test.cpp
        LIBRARIES
            dream::module_editor_panel_inspector
    )
    target_include_directories(dream_editor_panel_inspector_tests PRIVATE
        "${DreamEngine_SOURCE_DIR}/modules/editor_panel")
endif()
