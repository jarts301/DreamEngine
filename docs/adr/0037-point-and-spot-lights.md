# 0037. Point and spot lights, and forward_plus

- Status: Accepted
- Date: 2026-11-19

## Context

The only light was the sun. Interiors, night scenes, lamps, torches and effects need lights that
shine from a point, every way or in a cone, and a scene may have hundreds. A forward renderer that
loops over every light for every pixel costs too much with many lights, so the roadmap planned a
`forward_plus` renderer module.

Forward+ is usually tiled or clustered lighting with the lights sorted into parts of the screen by
a compute shader. The graphics interface has no compute shaders, on any of its six backends.

## Decision

### Lights as renderer data

`renderer::Light` is a point light or a spot light: position, direction for a spot, color,
intensity, range, and a spot's inner and outer angles. `Frame::lights` carries them. The runtime
gathers them from two components, `PointLight` and `SpotLight`, placed by their entity's transform,
a spot pointing along its entity's -Z axis as cameras look. Scene files write them as
`point_light` and `spot_light`.

A light's intensity is what a white matte surface facing it 1 m away reflects, in the sun's units.
Its light falls off with the square of the distance, times a window that brings it smoothly to
nothing at its range, and a spot's fades between its inner and outer angles. They don't cast
shadows.

### Clustered forward shading, built on the CPU

The renderer splits the view into clusters: boxes of the screen, at slices of depth that grow
logarithmically with distance. Each frame, on the CPU, it picks the nearest lights whose range is
in view, finds each one's clusters from its bounding sphere's corners on screen and its depths, and
writes the lights, each cluster's count and start, and the lists of light indices into a float
texture. The fragment shader finds its cluster from its world position, projected with the frame's
matrix, which every graphics API agrees on, and loops over that cluster's lights only.

Two modules are built from the same source and shaders, which a game listing both compiles once:

- **`forward_plus`**, with `DREAM_FORWARD_PLUS` defined: 16 x 9 clusters across the screen and 24
  slices of depth, and up to 256 lights.
- **`forward`**: one cluster, holding the 16 nearest lights.

`[renderer] max_lights` lowers either limit. `FrameStats` counts the lights drawn and those left
out.

### Data textures in the graphics interface

`TextureFormat::Rgba32Float` holds full floats, read texel by texel, never filtered. A texture
made with `TextureDesc::dynamic` has its rows replaced by `CommandList::update_texture`, between
render passes, on all six backends:

- Vulkan: a copy from the frame's upload memory, between barriers.
- Direct3D 11: `UpdateSubresource` on a default-usage texture.
- Direct3D 12: a copy from the frame's upload memory, at the pitch copies need, between barriers.
- OpenGL and OpenGL ES: `glTexSubImage2D`. OpenGL ES can't filter full floats, and treats a float
  texture with a filtering sampler as incomplete, so such a texture is read through no sampler
  object, by nearest settings of its own.

The lights' texture is 256 x 32 texels, of which a frame updates only the rows it uses. The forward
shader reads it at texture slot 1, which the tone-mapping pass uses only after the scene.

## Consequences

- Scenes can have lamps, torches and colored effects. The render demo has a lamp, a spot on its
  brick wall and a ring of eight small lights, drawn with `forward_plus`; its other checks' samples
  keep `forward`.
- Light assignment is CPU work each frame, which grows with lights times the clusters they cover.
  A compute pass would move it to the GPU once the interface has compute shaders.
- Clusters are conservative: a light reaches every cluster its bounding sphere's box touches,
  so a pixel may loop over a light that doesn't reach it, which the shader skips.
- Point and spot lights cast no shadows; shadowed spot lights, then point lights with cube maps,
  are later steps.
- Transparent parts are lit by them too, and so are skinned ones.
