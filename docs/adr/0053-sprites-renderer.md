# 0053. A renderer module for 2D games

- Status: Accepted
- Date: 2026-11-27

## Context

A 2D game drew with `forward`, the 3D renderer (ADR 0030), which draws sprites over the 3D scene
(ADR 0039). With no 3D scene, most of it went unused, but not free:

- the game shipped and loaded every 3D shader: lit, cut-out, skinned and shadow pipelines, the sky,
  tone mapping, bloom and particles, each compiled for every graphics backend it lists;
- `forward` made a shadow map, a float texture for lights rewritten each frame, and HDR targets
  sized to the window, which a 2D frame never touches.

The renderer is a module (ADR 0030), so a game can list another.

## Decision

A third module built from `forward_renderer.cpp`, as `forward_plus` is (ADR 0037): **`sprites`**,
built with `DREAM_SPRITES_RENDERER`.

- It draws the frame's sprites with the 2D camera, its debug lines, such as Physics2D's, and the
  interface layers, straight into the window, as `forward` already does when a frame has no 3D view.
  The pixels are the same.
- Its module lists only `sprites.slang`, `debug_lines.slang` and `ui.slang`, so a game listing it
  ships and loads three shaders instead of sixteen.
- It makes no shadow map, lights' texture or HDR targets, whatever the `[renderer]` settings say.
- A frame with models, particles or lights draws without them, and the log says once what was left
  out and to list `forward` to draw them.

In the shared source, `make_pipelines` makes the 2D pipelines every renderer of the file needs, and
`make_scene_pipelines`, `start_scene` and `render_scene` the 3D renderers' rest. `if constexpr` on
`k_sprites_only` chooses, so both builds compile all of it and `forward`'s behavior is unchanged:
its tests and every sample's picture pass as before.

`samples/top_down` lists `renderer = ["sprites", "null"]`, and its picture matches its reference
exactly on the five PC graphics backends. It runs on a Galaxy A57 (Xclipse 550) with Vulkan, and on
a Redmi Note 10S (Mali-G76) with Vulkan and OpenGL ES, whose two pictures are identical, as are the
platformer's there.

## Consequences

- A 2D game lists `sprites` and ships fewer shaders, and its renderer allocates nothing it doesn't
  use.
- A 2D game that wants particles, lit sprites or a 3D background lists `forward`, which draws the
  same sprites over its scene.
- The three renderers share one source, so a change to sprites, lines or the interface reaches all
  of them; the sprites module's tests check it loads only its shaders and makes no 3D targets.
- 2D particles, sprites lit by 2D lights, and post-processing for 2D, such as bloom, would be added
  to this module.
