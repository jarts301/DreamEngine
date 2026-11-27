# Samples

Sample games, written in C++20 and identical on every platform. Samples use the engine's public API
and never include platform or backend headers.

Each feature that can be seen has a sample of its own, small enough to read in one sitting: a
scene file for what the feature needs, and a short game that does only what the scene can't say.
Each compares a picture with a reference on all five PC graphics backends, and on WARP in CTest.

| Feature sample | Shows | Guide |
|---|---|---|
| [materials](materials) | Plastic and metal from mirror-smooth to fully rough, a normal-mapped wall, gold, blended glass, a cut-out fence and a glowing lamp | [Materials](../docs/guides/materials.md) |
| [lights](lights) | 240 colored point lights circling over a courtyard at night and spot lights sweeping it, with `forward_plus` | [Lighting](../docs/guides/lighting.md) |
| [shadows](shadows) | A low sun turning over a long avenue, shadowed to 60 m in four cascades, with a cut-out fence's lattice in its shadow | [ADR 0029](../docs/adr/0029-directional-shadows.md) |
| [environment](environment) | Spheres lit and reflecting an HDR sky, glowing lamps with bloom, and day, dusk and night | [Environment](../docs/guides/environment.md) |
| [particles](particles) | A campfire, a fountain, sparkles, snow, and explosions set off from code | [Particles](../docs/guides/particles.md) |
| [animation](animation) | Skinned tentacles playing clips at different speeds, backward, once, and fading between clips | [Animation](../docs/guides/animation.md) |
| [forest](forest) | 6,600 trees and rocks in a few dozen draws: instancing, culling, and levels of detail made at build time | [ADR 0032](../docs/adr/0032-instancing-and-lods.md) |
| [physics](physics) | Dominoes, a Newton's cradle on distance joints, a hinged gate, bouncy balls in a sensor, a laser of ray casts, and balls thrown where you click | [ADR 0011](../docs/adr/0011-physics.md) |
| [input](input) | A ball moved by keys, sticks, triggers or a finger, and a panel showing each action's state and strength | [Input](../docs/guides/input.md) |
| [ui](ui) | A title menu, settings with sliders and switches, and a HUD with health, score and timer, in RML and RCSS | [ADR 0012](../docs/adr/0012-audio-and-ui.md) |
| [platformer](platformer) | A 2D platformer: a tile map level with collisions, parallax hills and clouds, animated tiles, a 9-slice sign, an animated pixel-art hero moved by Box2D, coins and a following camera, on keyboard, gamepad and touch | [2D games](../docs/guides/sprites.md) |
| [top_down](top_down) | A top-down 2D garden made in the Tiled map editor: its layers, animated water, solid tiles and objects read by the `tiled` module, and a gardener who walks among trees sorted by height | [2D games](../docs/guides/sprites.md) |

Games and checks that use many features together:

| Sample | Shows |
|---|---|
| [arcade_demo](arcade_demo) | A small game using everything: physics, music and sounds, a HUD and menu, ImGui debug tools, and on PC a profile of each run, a memory budget and a crash you can switch on to try crash reports. Its checks run it with Jolt, miniaudio and RmlUi, and again with impulse, PortAudio and markup |
| [toppler](toppler) | The first complete game, a physics puzzle: knock every gold crate down within each level's throws, earning stars, with progress saved |
| [render_demo](render_demo) | Every rendering feature in one yard, drawn from a scene file |
| [physics_demo](physics_demo) | 3D and 2D physics from scene files, with shooting, sensors, joints and debug shapes; a scripted run checks the simulation with Jolt and Box2D and with impulse and Chipmunk |
| [scene_demo](scene_demo) | Scenes in TOML files, built from components, using shared assets and events, each scene shipped in a bundle of its own |
| [hello_game](hello_game) | A complete game on PC and Android: window, lifecycle, files and input actions. Checks run it with the profiler, SDL3 and a crash |
| [cube](cube) | A spinning cube drawn through the graphics interface alone, with Slang shaders, on every backend |
| [render_targets](render_targets) | Render targets through the graphics interface alone: a shadow map, a float color target, and a copy to the window |
| [template](template) | The project template that `dream new` copies, from `templates/game`, built and run here so the engine's tests catch a change that breaks new games |

## Making a feature sample

[cmake/FeatureSample.cmake](cmake/FeatureSample.cmake) builds a feature sample and its checks from
two files: `dream.toml`, the game's own configuration, and `tests/check.toml.in`, the checks', with
`@RHI@`, `@RHI_TABLE@`, `@SCREENSHOT@` and `@QUIT_AFTER@` filled in for each graphics backend:

```cmake
dream_add_feature_sample(lights
    SOURCES src/lights.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.lights
    DISPLAY_NAME "Dream Lights"
    NULL_PASS "frames=5 draws=[1-9][0-9]* lights=24[0-9] dropped=0"
    WARP
)
```

A check holds everything that moves at one moment, from a setting under `[demo]`, so its picture is
the same every run. The first run without a reference saves its picture beside the program and
fails; copy it to `tests/reference/screenshots/<name>.png` once it looks right. The 3D samples share
the orbiting camera in [common/sample_camera.h](common/sample_camera.h).

The models and sounds in [assets](assets) are shared by the samples, and generated by
[tools/make_sample_models.py](../tools/make_sample_models.py) and
[tools/make_sample_sounds.py](../tools/make_sample_sounds.py); [assets_3d](assets_3d) holds what
only the 3D samples share, the sky, the smoke and the feature samples' spheres and lamps, from
[tools/make_environment.py](../tools/make_environment.py),
[tools/make_particle_textures.py](../tools/make_particle_textures.py) and
[tools/make_feature_models.py](../tools/make_feature_models.py), which also makes the forest's trees
and rocks; the platformer's pixel art by
[tools/make_sprite_art.py](../tools/make_sprite_art.py), and the top-down garden's art and first map by
[tools/make_top_down_art.py](../tools/make_top_down_art.py). The fonts are Lato, under the
[SIL Open Font License](assets/fonts/LICENSE-Lato.txt).
