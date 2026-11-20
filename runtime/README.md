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
For that, list a renderer module, such as `renderer = ["forward"]`, under `[modules.<platform>]`.
See [ADR 0030](../docs/adr/0030-renderer-module.md). Listing a `mesh_simplifier` as well, such as
`mesh_simplifier = ["meshoptimizer"]`, gives models simpler versions for the renderer to draw far
away ([ADR 0032](../docs/adr/0032-instancing-and-lods.md)). Listing a `tangent_generator`, such as
`tangent_generator = ["mikktspace"]`, makes tangents for normal maps on models that have none
([ADR 0033](../docs/adr/0033-pbr-materials-and-normal-maps.md)); the
[materials guide](../docs/guides/materials.md) explains how to make models whose materials look
right. Input comes as named actions from `app.input()`, which `[input.actions]` binds to keys,
pointers and, with `input_device = ["sdl3_gamepad", "null"]` listed, gamepads
([ADR 0035](../docs/adr/0035-gamepads.md), and the [input guide](../docs/guides/input.md)).
`runtime::Animation` plays skeletal animation on entities with an `Animator`, which needs
`animation` listed ([ADR 0036](../docs/adr/0036-skeletal-animation.md), and the
[animation guide](../docs/guides/animation.md)). The `point_light` and `spot_light` components light
the scene besides the sun; list `renderer = ["forward_plus", "forward", "null"]` for many of them
([ADR 0037](../docs/adr/0037-point-and-spot-lights.md), and the
[lighting guide](../docs/guides/lighting.md)). `runtime::Particles` moves particle effects on
entities with a `ParticleEmitter`, which needs `particles` listed
([ADR 0038](../docs/adr/0038-particles.md), and the [particles guide](../docs/guides/particles.md)).
`dream/runtime/sprites.h` has the 2D camera, sprites, sprite animators and tile maps, which the
renderer draws and its scene components set up ([ADR 0039](../docs/adr/0039-sprites-and-tile-maps.md),
and the [2D games guide](../docs/guides/sprites.md)).
[samples/physics_demo](../samples/physics_demo/src/physics_demo.cpp) adds 3D and 2D physics with
`runtime::Physics3D` and `runtime::Physics2D`, which need `physics3d` or `physics2d` listed under
`[modules.<platform>]`.
[samples/arcade_demo](../samples/arcade_demo/src/arcade_demo.cpp) adds `runtime::Audio`,
`runtime::UserInterface` and `runtime::DebugOverlay`, which need `audio`, `ui` and `debug_ui`
listed the same way.

## Memory

The App watches memory as `[memory]` in the project configuration sets out, and calls
`Game::on_low_memory` when it runs low, on every platform. `App::memory_pressure` tells the level,
and the AssetManager refuses new assets while it is critical. See
[Memory budgets](../docs/reports/memory-budgets.md) and [ADR 0018](../docs/adr/0018-memory-budgets.md).

## Crashes

`run_program` installs a crash handler from `[crash_report]` in the project configuration, so a
crash leaves a report, and on Windows a minidump. `[crash_report] simulate` crashes on purpose to
try it. See [Crash reports](../docs/reports/crash-reports.md) and
[ADR 0019](../docs/adr/0019-crash-reports.md).

## Profiling

A game needs no code to be profiled: listing `profiler` under `[modules.<platform>]` makes
`run_program` start a `runtime::ProfilerSession`, and the runtime's own zones show the main loop and
each system. Frame timing, memory, CPU use and counts such as draw calls are recorded too. Add
the game's own zones and values with the macros in `dream/core/profile.h`. See
[ADR 0015](../docs/adr/0015-profiling.md), [ADR 0016](../docs/adr/0016-profiling-resources.md) and
[Reading profiles](../docs/reports/reading-profiles.md).
