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
`Depth32Float` shadow map, as the sun sees them. The forward shader then looks up how much of the
sun reaches each point and dims the direct light, and its highlight, by it. Ambient light is left
alone, so shadows are never black.

```toml
[renderer.shadows]
mode = "auto"         # "auto", "on" or "off".
resolution = 0        # The map's width and height; 0 picks one per device.
distance = 40.0       # How far from the camera shadows reach, in world units.
```

`DirectionalLight::casts_shadows` turns them off for a light, and `MeshRenderer::casts_shadows`
for a model, such as a ground plane that shadows nothing. Scene files take both.

### Where they are on

As decided for Phase 12: with `"auto"`, PCs get a 2048x2048 map. Phones get 1024x1024 when the
device can draw depth targets and has at least 4 GB of memory; phones with less carry the weakest
GPUs, where shadows would cost too much of the frame, and have none. A device that can't draw
depth targets has none anywhere. `"on"` turns them on wherever depth targets work, and the log
says what was chosen.

### The sun's view

An orthographic box around the part of the camera's view within `distance`, reaching back toward
the sun by as far again, so casters behind the camera still shadow what it sees. The box is the
bounding sphere of that part of the view, so its size doesn't change as the camera turns, and it
moves in whole texels of the map, so shadow edges hold still as the camera moves.

### Against surfaces shadowing themselves

- Casters are drawn with both faces, since models needn't be closed shapes, with a depth bias that
  grows with the slope.
- The forward shader looks a point up after moving it along its normal by one and a half texels of
  the map, in world units, and compares with a small constant bias.

### Soft edges

Nine filtered comparisons around each point, each one of the hardware's 2x2 comparison samples,
give edges a gradient about three texels wide. Outside the map, everything is lit.

## Consequences

- The render demo, the arcade and Toppler now have shadows. Their reference images were made
  again, and match on Vulkan, Direct3D 11 and 12, OpenGL, OpenGL ES and WARP.
- On a Galaxy A57, at 1080x2340 with HDR, the render demo's GPU time went from 2.68 to 3.88 ms a
  frame with a 1024x1024 map, and it still runs at 120 FPS.
- A 1024 map reaching 40 units shows stepped edges on a phone in portrait, where the camera sees
  a wide area. Cascades, two to four maps at growing distances, are the next step; a game can
  shorten `distance` in the meantime.
- Only the first directional light casts shadows. Point and spot lights, and their shadows, come
  with those lights.
