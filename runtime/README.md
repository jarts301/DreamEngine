# Runtime

The engine layer that games talk to: the main loop, entities, scenes, assets, events, virtual files,
input actions, the renderer, physics, audio and interfaces. It depends only on interfaces and Core,
and never names a backend. See [ADR 0007](../docs/adr/0007-platform-and-game-loop.md),
[ADR 0009](../docs/adr/0009-world-assets-scenes.md), [ADR 0011](../docs/adr/0011-physics.md) and
[ADR 0012](../docs/adr/0012-audio-and-ui.md).

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

[samples/hello_game](../samples/hello_game/src/hello_game.cpp) is a complete example,
[samples/scene_demo](../samples/scene_demo/src/scene_demo.cpp) adds scenes, assets and events, and
[samples/render_demo](../samples/render_demo/src/render_demo.cpp) draws a scene with the renderer.
For that, add `RENDERER` to `dream_add_game`. See [ADR 0010](../docs/adr/0010-renderer-v1.md).
[samples/physics_demo](../samples/physics_demo/src/physics_demo.cpp) adds 3D and 2D physics with
`runtime::Physics3D` and `runtime::Physics2D`, which need `physics3d` or `physics2d` listed under
`[modules.<platform>]`.
[samples/arcade_demo](../samples/arcade_demo/src/arcade_demo.cpp) adds `runtime::Audio`,
`runtime::UserInterface` and `runtime::DebugOverlay`, which need `audio`, `ui` and `debug_ui`
listed the same way.

## Profiling

A game needs no code to be profiled: listing `profiler` under `[modules.<platform>]` makes
`run_program` start a `runtime::ProfilerSession`, and the runtime's own zones show the main loop and
each system. Add the game's own with the macros in `dream/core/profile.h`. See
[ADR 0015](../docs/adr/0015-profiling.md).
