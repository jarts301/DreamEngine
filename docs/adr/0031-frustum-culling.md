# 0031. Frustum culling

- Status: Accepted
- Date: 2026-11-18

## Context

The forward renderer drew every part of every model each frame. On top of that, it drew every
shadow caster into every cascade, four times on a PC and three on a phone. A scene larger than one
screen paid for all of it, on the CPU, which records each draw, and on the GPU, which transforms
every vertex before clipping throws it away.

## Decision

Each model part has a box around its vertices, in model space. `create_model_asset` works it out
once, when the model loads, and `ModelPart::bounds` keeps it. The runtime passes it to the renderer
in `renderer::MeshDraw::bounds`. A part with an empty box is treated as having an unknown extent
and is always drawn, so data from elsewhere without boxes still works.

Core gains `core::Aabb`, `core::transformed`, `core::Frustum`, `core::frustum_from` and
`core::intersects` (`dream/core/math/bounds.h`). These are plain math that other modules, such as
physics or a future `visibility` interface, can share.

The forward renderer, when it has a view, moves each part's box into the world once per frame, then
tests it:

- against the camera's frustum, before drawing it into the scene;
- against each cascade's box, before drawing it into that cascade's part of the shadow map. Casters
  between the camera and the sun stay in, since the cascade boxes already reach back toward the
  sun.

Culling only skips draws that would have drawn nothing, so pictures don't change. `FrameStats`
reports `culled` and `shadow_culled`, the profiler shows them, and `Renderer::last_culled_count`
gives the first. It can be turned off for comparison:

```toml
[renderer]
culling = true   # The default.
```

Culling is a renderer feature, not a module of its own. How a renderer decides what to draw is
part of how it draws, and the renderer is already a module (ADR 0030), so a project that wants a
different approach swaps the renderer. A `visibility` interface can come later, for spatial
structures or occlusion culling that several renderers would share.

## Consequences

- Draws out of view cost one box transform and up to six plane tests, instead of a draw call.
- Boxes are tested one by one. Scenes with tens of thousands of parts will want a hierarchy, such as
  a BVH, which is the `visibility` interface's job.
- The test is conservative: a box near a corner of the frustum can pass while outside it. That only
  costs a draw.
- Boxes come from vertices as loaded. Anything that moves vertices in a shader, such as skinning or
  wind, will need larger boxes when it arrives.
