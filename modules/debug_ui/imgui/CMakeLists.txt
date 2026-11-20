# Built from the sources in third_party/ (ADR 0039).
dream_add_module(debug_ui imgui SOURCES src/imgui_debug_ui.cpp LIBRARIES imgui::imgui)

if(DREAM_BUILD_TESTS)
    dream_add_test(dream_debug_ui_imgui_tests
        SOURCES
            tests/imgui_test.cpp
        LIBRARIES
            dream::module_debug_ui_imgui
    )
    target_include_directories(dream_debug_ui_imgui_tests PRIVATE
        "${DreamEngine_SOURCE_DIR}/modules/ui")
endif()
