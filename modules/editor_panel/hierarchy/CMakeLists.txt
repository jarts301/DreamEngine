dream_add_module(editor_panel hierarchy SOURCES src/hierarchy_panel.cpp)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_editor_panel_hierarchy_tests
        SOURCES
            tests/test_main.cpp
            tests/hierarchy_panel_test.cpp
        LIBRARIES
            dream::module_editor_panel_hierarchy
    )
    target_include_directories(dream_editor_panel_hierarchy_tests PRIVATE
        "${DreamEngine_SOURCE_DIR}/modules/editor_panel")
endif()
