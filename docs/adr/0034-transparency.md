# 0034. Transparent, cut-out and double-sided materials

- Status: Accepted
- Date: 2026-11-19

## Context

ADR 0033 gave models glTF's metallic-roughness materials, but drew every surface opaque: glTF's
`alphaMode` and `doubleSided` were ignored. Glass, water and smoke need blending; leaves, fences,
grass and decals need holes cut by their texture's alpha; thin surfaces such as leaves and cloth
need both faces drawn.

Emission was already there (ADR 0033): emissive color and texture, and
`KHR_materials_emissive_strength`.

## Decision

`renderer::MeshMaterial` gains glTF's `alpha_mode`, `alpha_cutoff` and `double_sided`, which
`runtime::parse_glb` reads. Base color alpha is its factor times its texture's.

### Passes

The forward renderer draws the scene's parts in three passes, each with its own pipelines:

1. **Opaque** parts, grouped into instanced draws as before (ADR 0032).
2. **Mask** parts, the same way, with `forward_masked.slang`: `forward.slang` with
   `DREAM_ALPHA_MASK` defined, which discards pixels whose alpha is below the cutoff. The discard
   stays out of the opaque shader, because a shader that can discard stops many phone GPUs from
   testing depth before shading every surface drawn with it.
3. **Blend** parts, sorted farthest first by the middle of their boxes, blended with premultiplied
   alpha, testing depth without writing it. Their diffuse light is multiplied by alpha, while their
   reflections and emission are not, as glass reflects the sky however clear it is.

Each pass has a one-sided pipeline, culling back faces, and a double-sided one, culling none. The
shader lights a double-sided surface's back face as its front turned around, telling which face
it sees from the normal and the direction to the camera, which holds on every graphics API.

The instance data carries the alpha cutoff in the w of the normals' first column, which was
unused, and two more feature bits (ADR 0033): double-sided, and blended.

### Shadows

Opaque parts cast shadows as before. Mask parts cast theirs in a second shadow draw per cascade,
with `shadow_masked.slang`, which reads the base color texture and discards below the cutoff, so a
fence casts the shadow of its lattice. Blend parts cast none.

### Shader dependencies

`forward_masked.slang` includes `forward.slang`. The build now compiles each of a module's
shaders again when any of them changes, so an included file is never left stale.

## Consequences

- glTF's three alpha modes and double-sidedness draw as the format describes. The render demo
  has a glass pane, a cut-out fence and a glowing lamp, and its reference picture was redone.
- Blended parts are sorted per part, not per triangle, so intersecting or overlapping blended
  parts can blend in the wrong order, and a concave blended model may show its own far side over
  its near one. Order-independent transparency would remove that, and is left for later.
- Blended parts cast no shadow, and receive the sun's shadow like any surface.
- The render demo's picture matches on all five PC backends, and the Galaxy A52s (Adreno 642L)
  draws the same with Vulkan and OpenGL ES, blending into its R11G11B10 float target. Every
  Vulkan check runs clean under the validation layers.
- Alpha-tested edges alias, as there is no multisampling or alpha-to-coverage yet.
- Emissive surfaces still light nothing around them, and there is no bloom.
