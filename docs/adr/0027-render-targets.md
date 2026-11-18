# 0027. Render targets, texture slots and depth-only pipelines

- Status: Accepted
- Date: 2026-11-18

## Context

The graphics interface drew only to the window and read one texture. Phase 12's rendering upgrade
needs more: HDR drawing that is tone-mapped onto the window, and shadow maps that are drawn from
the light and read while drawing the scene. Both need passes that draw into textures, shaders
that read more than one texture, a depth comparison, and pipelines that write only depth. Every
backend has to do it alike: Vulkan, Direct3D 11 and 12, OpenGL, OpenGL ES and the null device.

## Decision

### Render targets

A texture made with `TextureDesc::render_target` can be drawn into and then read:

```cpp
const rhi::TextureHandle color = device.create_texture(
    {.width = 1280, .height = 720, .format = rhi::TextureFormat::Rg11B10Float,
     .render_target = true}, {});

commands.begin_render_pass(rhi::RenderTargets{.color = color, .depth = depth});
// ... draws ...
commands.end_render_pass();
```

- Three formats are for render targets only: `Rgba16Float` and `Rg11B10Float` for colors brighter
  than 1, and `Depth32Float` for depth that shaders read. `Device::supports_render_target` says
  which a device can draw into and filter; the 8-bit formats always work. Phase 12 picks the HDR
  format per device from it.
- A pass clears its targets, or keeps what they had when the clear value is left out.
- A render target has one level and starts undefined. A pass can't read its own targets.
- Pixels are addressed from the top left in every pass, so a texture's first row is the top of
  what was drawn, on every API.

### Texture slots

Shaders read up to three textures, each with a fixed sampler:

| Slot | Sampler | Vulkan | DirectX | OpenGL |
|---|---|---|---|---|
| 0 | linear, mipmaps, repeating | set 0, binding 1 | `t0`, `s0` | texture unit 1 |
| 1 | linear, clamped to the edge | set 0, binding 2 | `t1`, `s1` | texture unit 2 |
| 2 | depth comparison, less or equal, linear, clamped | set 0, binding 3 | `t2`, `s2` | texture unit 3 |

`bind_texture(slot, texture)` binds one; `bind_texture(texture)` is slot 0, as before. Slot 2, the
shadow slot, takes `Depth32Float` textures, and a shader reads it with `Sampler2DShadow` and
`SampleCmp`. Every pass starts with defaults: white for slots 0 and 1, and a depth at the far
plane for slot 2, so a scene with no shadow map is fully lit. In Slang:

```hlsl
[[vk::binding(3, 0)]]
Sampler2DShadow shadow_map : register(t2) : register(s2);
```

The backends check a shader's textures and samplers against the table, as they check its
constant buffers (ADR 0013).

### Pipelines

`PipelineDesc` gained:

- `color_target` and `depth_target`: the formats of the targets the pipeline draws into, or
  neither for the window. Vulkan's and Direct3D 12's pipelines are built for their targets, so a
  pipeline is only bound in passes with those formats, which every backend checks.
- An optional fragment shader, for pipelines with only a depth target, such as shadow casters.
- `cull`, which can now cull front faces, as shadow maps often do, replacing `cull_back_faces`.
- `depth_bias` and `slope_depth_bias`, to keep surfaces from shadowing themselves.

### How each backend does it

- **Vulkan** makes a render pass per combination of target formats and clear choices, and a
  framebuffer per pair of targets. Targets wait in the shader-read layout; the render pass moves
  them to the attachment layouts and back, and its dependencies order it after earlier passes and
  before later reads. The comparison sampler filters linearly where `D32_SFLOAT` can, and takes
  the nearest texel elsewhere.
- **Direct3D 12** transitions a pass's targets to render-target or depth-write state and back to
  shader-resource state at its end. Each slot is its own one-descriptor table, and the samplers are
  static. Depth targets are typeless, with a `D32_FLOAT` view to draw and an `R32_FLOAT` view to
  read, as on Direct3D 11.
- **Direct3D 11** unbinds the textures before a pass and the targets after it, so nothing is read
  and drawn at once.
- **OpenGL** stores a texture's rows from the bottom, the other APIs from the top. The build adds a
  `dream_clip_flip` uniform to every vertex shader (ADR 0026), which the device sets to -1 in passes
  into textures, turning the picture over, with the triangles' winding reversed to match. A
  pipeline that draws into textures without that uniform is refused. Depth-only programs get an
  empty fragment shader, since OpenGL ES links only programs with both stages.
- **The null device** checks the same rules: targets of the right kinds and sizes, pipelines in
  passes with their formats, and the shadow slot.

## Consequences

- `samples/render_targets` draws a shadow map from a light, the scene into an `Rg11B10Float`
  target lit and shadowed by it, and the result onto the window. Its screenshot matches one
  reference image on Vulkan, Direct3D 11 and 12, OpenGL, OpenGL ES and both WARP checks on an RTX
  5080, and it looks the same on a Galaxy A57 with Vulkan and with OpenGL ES. The WARP checks run
  in CTest.
- The device tests of Direct3D 11, Direct3D 12 and both OpenGL backends draw into textures, check
  that the top row comes first and that scissors work there, draw depth only, and compare against
  it. Vulkan has no device tests; the sample's check covers it.
- Breaking for code that used `cull_back_faces`: it is now `cull = rhi::CullMode::None` or `Back`.
- Targets are single-sampled, and there is one color target per pass. Multisampling and several
  color targets can come later through the same structures.
