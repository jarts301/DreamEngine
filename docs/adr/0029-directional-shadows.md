# 0029. Shadows from the directional light

- Status: Accepted
- Date: 2026-11-18

## Context

The renderer lit everything the sun faced, whatever stood between them, so objects floated over
the ground and stacks had no depth. Render targets, a depth-only pipeline and a comparison sampler
(ADR 0027) make a shadow map possible on every backend. Phones vary far more than PCs in what they
can afford, so where shadows are on by default had to depend on the device.

## Decision

Each frame, before the scene, the renderer draws every model that casts shadows into a
`Depth32Float` shadow map, as the sun sees them, once for each cascade: a part of the view at a
growing distance from the camera, so near shadows get as much of the map as far ones over a much
smaller area. The forward shader then looks up how much of the
sun reaches each point and dims the direct light, and its highlight, by it. Ambient light is left
alone, so shadows are never black.

```toml
[renderer.shadows]
mode = "auto"         # "auto", "on" or "off".
resolution = 0        # Each cascade's width and height; 0 picks one per device.
cascades = 0          # From 1 to 4; 0 picks one per device.
distance = 40.0       # How far from the camera shadows reach, in world units.
```

`DirectionalLight::casts_shadows` turns them off for a light, and `MeshRenderer::casts_shadows`
for a model, such as a ground plane that shadows nothing. Scene files take both.

### Where they are on

As decided for Phase 12: with `"auto"`, PCs get four cascades of 2048x2048. Phones get three of
1024x1024 when the device can draw depth targets and has at least 4 GB of memory; phones with
less carry the weakest GPUs, where shadows would cost too much of the frame, and have none. A device that can't draw
depth targets has none anywhere. `"on"` turns them on wherever depth targets work, and the log
says what was chosen.

### Cascades

The shadowed part of the view, out to `distance`, is split among the cascades, near ones short and
far ones long: each split is 70% of the way from even steps to logarithmic ones. Each cascade's
view of the sun is an orthographic box around its part of the camera's view, reaching back toward
the sun by `distance` more, so casters behind the camera still shadow what it sees. The box is the
bounding sphere of that part, so its size doesn't change as the camera turns, and it moves in
whole texels of its map, so shadow edges hold still as the camera moves.

The graphics interface has one shadow slot and no viewports, so the cascades share one texture,
side by side: two across, then two down. Each cascade's matrix squeezes its box into its cell of
clip space, and a scissor keeps its casters there. The forward shader picks the first cascade
whose part of the view holds the point, by its depth along the camera's view, and keeps its nine
samples inside that cell. The frame's uniform block grew past 256 bytes with four matrices, so the
interface's limit, `k_max_uniform_size`, is now 512, far below what every API guarantees.

### Against surfaces shadowing themselves

- Casters are drawn with both faces, since models needn't be closed shapes, with a depth bias that
  grows with the slope.
- The forward shader looks a point up after moving it along its normal by one and a half texels of
  its cascade, in world units, and compares with a small constant bias.

### Soft edges

Nine filtered comparisons around each point, each one of the hardware's 2x2 comparison samples,
give edges a gradient about three texels wide. Outside the map, everything is lit.

## Consequences

- The render demo, the arcade and Toppler now have shadows. Their reference images were made
  again, and match on Vulkan, Direct3D 11 and 12, OpenGL, OpenGL ES and WARP.
- On a Galaxy A57, at 1080x2340 with HDR, the render demo's GPU time went from 2.76 to 4.10 ms a
  frame with three cascades of 1024x1024, and it still runs at 120 FPS. Two cascades cost the same,
  since the casters are few and the lookup is the same nine samples, but left the demo's objects
  in the far cascade, with stepped edges: three put them in a nearer one.
- Cascades replaced a single map, which at 1024x1024 over 40 units showed stepped edges on a phone
  in portrait, where the camera sees a wide area.
- Where two cascades meet, the texel size changes at once; blending across the seam would hide it,
  at the cost of a second set of samples there.
- Only the first directional light casts shadows. Point and spot lights, and their shadows, come
  with those lights.
- On Adreno GPUs (seen on a Galaxy A52s's Adreno 642L, through Vulkan), the driver misread the
  `float4x4 cascades[4]` array in the frame's uniform block. Every point then looked lit, so there
  were no shadows, while single matrices read correctly. The shaders now declare the same bytes as
  16 `float4` columns and multiply by them directly. Arrays of matrices in uniform blocks are best
  avoided in this engine's shaders.
