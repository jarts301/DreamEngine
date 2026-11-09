# Runtime

The engine layer that games talk to: the main loop, virtual files and input actions. It depends only
on interfaces and Core, and never names a backend. See
[ADR 0007](../docs/adr/0007-platform-and-game-loop.md).

## Writing a game

Derive from `runtime::Game`, define `runtime::create_game`, and add it with one CMake call:

```cmake
dream_add_game(my_game
    SOURCES src/my_game.cpp
    CONFIG dream.toml
    ASSETS assets
    APPLICATION_ID com.example.mygame   # For Android.
    DISPLAY_NAME "My Game"
)
```

[samples/hello_game](../samples/hello_game/src/hello_game.cpp) is a complete example.
