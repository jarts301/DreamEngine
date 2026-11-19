# 0032. Instancing and levels of detail

- Status: Accepted
- Date: 2026-11-19

## Context

After culling (ADR 0031), the forward renderer still recorded one draw for every visible part, and
another for every cascade it cast a shadow into. A scene of a hundred crates cost a hundred draws
in the scene, and up to four hundred more in its shadows, though every crate draws the same
indices with the same texture. A part far away was drawn with all of its triangles, though it
covers a few pixels.

## Decision

### Instancing

The graphics interface gains per-instance vertex data, on every backend:

- `PipelineDesc::instance_stride` and `instance_attributes` describe attributes read once per
  instance. They take locations after the per-vertex ones, which on DirectX means they come after
  them in the shader's inputs.
- `CommandList::bind_instances` copies one block per instance into the frame's vertex memory,
  which every per-frame upload shares. `k_max_frame_vertex_bytes` went from 2 MB to 4 MB to make
  room.
- `CommandList::draw_indexed_instanced` draws a range of indices once per instance.

Vulkan reads instances from vertex binding 1, at the instance rate. DirectX reads them from input
slot 1, as per-instance data. OpenGL and OpenGL ES read them from vertex buffer binding 1, with a
divisor of 1.

The forward renderer no longer uses push constants for parts. Each part's model matrix, normal
matrix and color are 128 bytes of instance data. After culling, the parts of a pass are sorted by
what they draw: their buffers, their range of indices and, outside the shadow pass, their texture.
Each run of equal parts becomes one instanced draw, of up to 1024 instances. The shadow pass groups
by mesh alone, so every crate in a cascade is one draw. `FrameStats::draw_calls` counts the draws
recorded.

### Levels of detail

Simpler versions of a part are more index ranges over the same vertices, appended to the model's
index buffer. They come with the part as `ModelPart::lods` and `renderer::MeshDraw::lods`. Each
version records its geometric error, which is how far its surface strays from the full part, in
model units.

The renderer picks a version per part per frame, from the error it would show on screen. That
error is the version's error, scaled by the model matrix and projected at the part's nearest point
to the camera. It draws the simplest version whose error stays within `lod_error` pixels:

```toml
[renderer]
lod_error = 1.0   # Pixels. 0 always draws parts in full.
```

Shadows use the version chosen for the camera.

Making the versions is a feature, so it's a module. The new `mesh_simplifier` interface takes a
mesh and a target and returns simpler indices with their error:

- **`meshoptimizer`** uses meshoptimizer (MIT). It keeps the edges where a model's parts meet in
  place, so they don't open gaps.
- **`null`** returns the mesh unchanged.

When a project lists a simplifier, the runtime makes the versions as each model loads. A project
that lists none gets none:

```toml
[modules.windows]
mesh_simplifier = ["meshoptimizer", "null"]

[lods]
levels = 3          # Versions per part, at most; 0 makes none.
reduction = 0.5     # Each aims for this fraction of the last one's triangles.
min_triangles = 64  # Smaller parts get none.
```

Each version is made from the full part, not from the previous version, so errors don't add up. A
version that saves less than a fifth of the last one's triangles ends the list.

## Consequences

- Repeated parts cost one draw per pass instead of one each. The samples' pictures are unchanged on
  every backend.
- Instances carry the color, so tinted copies of a model still group together. Parts with
  different textures don't.
- Making versions costs time when a model loads. Cooking them into the asset at build time would
  remove that, and is left for later.
- Versions are chosen per part, so a model's parts can switch at different distances. Their shared
  edges are kept, so they still meet.
- Switching between versions pops. Blending between them, by dithering or by morphing, can come
  later.
- Artist-made versions, such as glTF's MSFT_lod, aren't read yet. They would fill the same
  `ModelPart::lods`.
