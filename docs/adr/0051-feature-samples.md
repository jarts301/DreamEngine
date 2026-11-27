# 0051. A sample for each feature

- Status: Accepted
- Date: 2026-11-27

## Context

Most of the engine's visible features were shown only in `samples/render_demo`, whose yard holds
everything at once: materials, lights, shadows, the sky, a campfire and an animated tentacle. That
suits a check that nothing broke, but not someone learning one feature: its scene and code mix them
all, and its one picture can't show a feature's range, such as roughness from smooth to rough or
256 lights.

Meanwhile `samples/module_demo` and the `greeter` interface, kept to prove the module system before
real interfaces existed (ADR 0006), had nothing left to prove: every interface since does the same.

## Decision

### One sample per feature that can be seen

| Sample | Feature |
|---|---|
| `materials` | PBR materials, from smooth to rough, plastic and metal; normal maps, glass, cut-outs, emission |
| `lights` | 240 point lights and four spot lights, made and moved from code, with `forward_plus` |
| `shadows` | Cascaded sun shadows reaching 60 m, as the sun turns |
| `environment` | Environment lighting and bloom, at day, dusk and night |
| `particles` | An effect from each row of the particles guide, and bursts from code |
| `animation` | Clips at other speeds, backward, once, and fading between clips |
| `forest` | Instancing, culling and levels of detail made at build time, over 6,600 objects |
| `physics` | Joints, restitution, a sensor, ray casts, and bodies made from code |
| `input` | Actions, their analog values and last source, shown live |
| `ui` | A title menu, settings and a HUD in RML and RCSS |

`platformer` and `top_down` already were the 2D samples. Each sample is a scene file holding what
the feature needs and a short game doing only what a scene can't say, so the two read as the
feature's usage. `render_demo`, `arcade_demo`, `physics_demo` and the other existing samples stay,
since their checks cover the features together.

### Checks from one template

`samples/cmake/FeatureSample.cmake` gives `dream_add_feature_sample`, which builds the game from its
`dream.toml` and its checks from one `tests/check.toml.in`, filled in for each graphics backend: a
picture compared on Vulkan, DirectX 11 and 12, OpenGL and OpenGL ES, the same on WARP in CTest, and
a run on the `null` backends in CTest whose report must match a pattern. An existing sample needed
seven or eight test configurations differing in a line or two; a feature sample has one.

A check holds everything that moves at one moment from a `[demo]` setting, so its picture is the
same every run: animation and lights at a time, particles warmed up in fixed steps, physics stepped
a fixed number of times as the game starts.

### Shared pieces

- `samples/common/sample_camera.h` orbits the camera of the 3D samples with keys, sticks or a drag.
- `samples/assets_3d` holds what only the 3D samples share: the sky, the smoke texture, and the
  spheres and lamps made by `tools/make_feature_models.py`. They moved out of `render_demo`, and
  out of `samples/assets`, which every sample, 2D ones included, packages.
- The runtime `Renderer` gained `last_draw_call_count()` and `last_simplified_count()`, which the
  renderer already counted, so `forest` can show instancing and levels of detail at work.

### module_demo and greeter removed

The core's unit tests cover choosing a module from a list and falling back when one fails to start.
The READMEs name `texture_decoder` as the small example of an interface and a module.

## Consequences

- A feature is learned from a sample of 100 to 250 lines and one scene file.
- Each sample's picture is checked on every PC backend and WARP; the ten were also run on a Galaxy
  A52s (Adreno 642L) with Vulkan and OpenGL ES, whose pictures differ by at most 0.03% of pixels,
  and on a Galaxy A57 (Xclipse 550) with Vulkan.
- On a phone held upright the 3D samples' views are widened to fit, so things placed at the edges
  for landscape, such as the materials sample's lamp, can be cut off.
- A new feature that can be seen gets a sample of its own, made with `dream_add_feature_sample`.
