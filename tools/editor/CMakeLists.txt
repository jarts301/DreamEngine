# The editor (ADR 0046): a program built on the engine, like a game, whose panels are editor_panel
# modules. It runs on the build machine, so it is only built when the build is for that machine,
# and `cmake --install` puts it, with its assets, in the SDK's editor folder.
set(editor_sources src/editor.cpp src/editor_scene.cpp src/editor_process.cpp)

dream_add_game(dream_editor
    SOURCES ${editor_sources}
    CONFIG editor.toml
)
target_include_directories(dream_editor PRIVATE src)

if(DREAM_INSTALL_SDK)
    # The program and its staged assets, which sit beside it.
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dream_editor/" DESTINATION editor
        USE_SOURCE_PERMISSIONS
        PATTERN "*.pdb" EXCLUDE
        PATTERN "*.ilk" EXCLUDE)
endif()

if(DREAM_BUILD_TESTS)
    # The scene model the panels edit, on the engine's fakes.
    dream_add_test(dream_editor_tests
        SOURCES
            tests/editor_scene_test.cpp
            src/editor_scene.cpp
            "${DreamEngine_SOURCE_DIR}/runtime/tests/test_main.cpp"
        LIBRARIES
            dream::runtime
            dream::interface_editor_panel
    )
    target_include_directories(dream_editor_tests PRIVATE src
        "${DreamEngine_SOURCE_DIR}/runtime/tests")

    # The editor on the GPU with the template's crate selected, saving a picture. Not a CTest,
    # because build servers have no GPU: run it with the template's folder.
    dream_add_game(dream_editor_vulkan_check SOURCES ${editor_sources} CONFIG tests/vulkan.toml)
    target_include_directories(dream_editor_vulkan_check PRIVATE src)

    # Play, scripted: builds and starts the game of the project it is given, moves the template's
    # crate and saves while the game runs, then quits. Not a CTest: it builds a whole game.
    dream_add_game(dream_editor_play_check SOURCES ${editor_sources} CONFIG tests/play.toml)
    target_include_directories(dream_editor_play_check PRIVATE src)

    # Opens the project template's scene, on the null backends, and quits after a few frames.
    dream_add_game(dream_editor_null SOURCES ${editor_sources} CONFIG tests/null.toml)
    target_include_directories(dream_editor_null PRIVATE src)
    add_test(NAME dream_editor_null
        COMMAND dream_editor_null "${DreamEngine_SOURCE_DIR}/templates/game")
    set_tests_properties(dream_editor_null PROPERTIES
        PASS_REGULAR_EXPRESSION "editor frames=5 entities=4 problem=none"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
