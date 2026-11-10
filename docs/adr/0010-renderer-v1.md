# 0010. Renderer v1

- Status: Accepted
- Date: 2026-11-09

## Decision

### Where it lives

`runtime::Renderer` is ordinary Runtime code built on the graphics interface, so it works with any
backend. A game owns one and calls it from `on_render`. A game that wants different drawing can skip
it and use the graphics interface directly, as `samples/cube` does.

### What it draws

Forward rendering of opaque models with one directional light, ambient light, a Blinn-Phong
highlight and a base color texture. Output is linear, and the window's sRGB format encodes it.

Scene files use the components `camera`, `mesh` and `directional_light`. On a portrait screen the
camera widens its view, so a scene framed for landscape isn't cropped at the sides.

### Models

Models are binary glTF (`.glb`) files read with cgltf, and images are decoded with stb_image. Node
transforms are baked into the vertices, so one model is one vertex buffer plus a list of parts.

Parsing produces plain data before anything touches the GPU, so the loader is tested without one.

### Graphics interface additions

- **Textures**, with mipmaps generated on upload.
- **`set_uniforms`**, which copies data into a per-frame ring, so callers manage no buffers.
- **`bind_texture`**, where an invalid handle selects a built-in white texture, so untextured
  materials use the same shader.

Every pipeline shares one layout: a uniform block, a texture and push constants. That's enough for
one material model, and a richer layout can arrive when a feature needs it.

Vertex, index and texture data now go through staging uploads into GPU-only memory. Each upload
waits for the GPU, which is fine at load time.

### Shaders

The `RENDERER` option on `dream_add_game` compiles `runtime/shaders/forward.slang` into that game's
assets, so games that don't use the renderer carry nothing extra.

## Consequences

- There are no shadows, transparency, multiple lights, normal maps or culling yet. These can be
  added behind `Renderer::render` without changing how games use it.
- glTF animation and skinning are ignored.
- Loading a model blocks the frame.
- Each frame can use at most 512 different textures, which a later bindless design could lift.
