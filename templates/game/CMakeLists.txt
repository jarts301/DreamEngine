# __GAME_TITLE__, built with DreamEngine's SDK (see README.md).
cmake_minimum_required(VERSION 3.25)
project(__GAME_NAME__ VERSION 0.1.0 LANGUAGES CXX)

# The engine, from the SDK that DreamEngine_DIR names: the presets set it from DREAM_ENGINE_SDK.
find_package(DreamEngine __ENGINE_VERSION__ REQUIRED)

# The game: its code, its configuration, and its assets, which the build cooks and stages beside
# the program. On Android, the __GAME_NAME___apk target packages it.
dream_add_game(__GAME_NAME__
    SOURCES src/__GAME_NAME__.cpp
    CONFIG dream.toml
    ASSETS assets
    APPLICATION_ID __APPLICATION_ID__
    DISPLAY_NAME "__GAME_TITLE__"
)
