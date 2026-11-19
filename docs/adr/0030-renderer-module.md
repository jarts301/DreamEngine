# 0030. The renderer is a module

- Status: Accepted
- Date: 2026-11-18

## Context

The engine's rule is that every feature is a module chosen in `dream.toml`, and only the core that
every game needs stays fixed. The renderer broke that rule. All of its drawing lived in
`runtime::Renderer`: forward lighting, HDR and tone mapping (ADR 0028), cascaded shadows
(ADR 0029), debug lines and the interface pass. Its shaders sat in `runtime/shaders`, and a game
opted in with a `RENDERER` option on `dream_add_game`. A project couldn't swap how its scene is
drawn without editing the runtime. A game without a 3D scene, such as a server, still carried the
renderer's code.

## Decision

Drawing moves behind a new interface, `renderer::SceneRenderer`, in `interfaces/renderer`. It has
two modules:

- **`forward`** (`modules/renderer/forward`) is the renderer as it was, with the same settings,
  passes and output.
- **`null`** (`modules/renderer/null`) only clears the window.

A project lists them like any other module:

```toml
[modules.windows]
rhi = ["vulkan", "directx12", "null"]
renderer = ["forward", "null"]
```

The interface takes plain data, never the world. Each frame `runtime::Renderer` gathers a
`renderer::Frame` from the world and hands it over:

- the camera's view;
- the first sun;
- one `MeshDraw` for each part of each model: its buffers, index range, matrix, color, texture and
  whether it casts shadows;
- the debug lines;
- the interface layers, with a callback that turns interface texture ids into device textures.

The module records the whole frame. It begins and ends its own passes and returns counts for
profiling. Entities, components, scene files, model loading and interface textures stay in the
runtime. They are core, and every renderer shares them. The vertex layouts are shared through the
interface: `ModelVertex` is `renderer::MeshVertex`, and `DebugVertex` is `renderer::LineVertex`.

A module brings its own shaders. `dream_add_module` takes `SHADERS`, and `dream_add_game` compiles
the shaders of every module a game links, for each of its graphics backends. The `RENDERER` option
is gone. A module reads its compiled shaders through the `FileReader` it is given, so `forward`
loads them from `assets:/shaders/` as before. The runtime loads them as one asset,
`RendererShaders`, so hot reloading still rebuilds the pipelines when a shader file changes. A
failed reload keeps the old ones.

The games' API is unchanged: `Renderer::initialize`, `render`, `debug_lines`, `set_ui_layer`,
`ui_host` and the `last_*` counts. `initialize(app, backend)` takes a module directly, for tests.
`Renderer::settings()` is gone, because the settings belong to the module. `describe()` gives the
line the log prints, such as the HDR format and the shadows chosen.

## Consequences

- A project chooses its renderer in `dream.toml`. New renderers, such as a deferred one or a 2D
  one, are new modules, and need no runtime changes.
- Games that don't draw a scene don't list a renderer, and carry neither its code nor its
  shaders.
- `null` draws neither the scene nor the interface, so a game whose interface must show needs
  `forward`.
- The forward renderer's tests moved into the module (`dream_renderer_forward_tests`). The
  runtime's tests check what the runtime gathers, against a fake module.
- Each `MeshDraw` copies a model matrix and color per part per frame. That costs little next to
  the draw itself, and the runtime reuses the array between frames.
