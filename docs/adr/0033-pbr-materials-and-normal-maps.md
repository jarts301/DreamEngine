# 0033. PBR materials and normal maps

- Status: Accepted
- Date: 2026-11-19

## Context

The forward renderer lit every surface the same way: a base color, Lambert diffuse light and a
fixed Blinn-Phong highlight. glTF models carry metallic-roughness materials with normal,
occlusion and emissive maps, and the loader dropped everything but the base color. Models looked
alike whatever their artists made, and detail baked into normal maps was lost.

Normal maps need tangents. glTF files may have them, and when they don't, glTF asks for
MikkTSpace's, which is what baking tools use. Making tangents is a step of its own, which some
projects don't need.

The graphics interface had three texture slots, and the build cooked every texture as sRGB, which
is wrong for normal and roughness maps.

## Decision

### Materials in the renderer interface

`renderer::MeshMaterial` is glTF's metallic-roughness material: base color, metallic, roughness,
normal scale, occlusion strength and emissive color, each with an optional texture. It replaces
`MeshDraw::base_color` and `texture`. `runtime::ModelMaterial` is the same type. A mesh's tint
still multiplies the base color.

`renderer::MeshVertex` gains a tangent, a `Vec4` whose `w` gives the bitangent's direction, all
zero when unknown. Vertices are 48 bytes.

### Shading in `forward`

`forward.slang` lights the sun with a Cook-Torrance BRDF: GGX distribution, Smith's
height-correlated visibility in Hammon's approximation and Schlick's Fresnel, over Lambert diffuse, with dielectrics
reflecting 4%. Ambient light keeps its single value, now split into diffuse and an analytic
specular fit (Karis's mobile approximation), darkened by occlusion. Emission is added last. The
sun's units are unchanged: a white, fully rough dielectric facing it shows its color times its
intensity.

Each part's factors are per-instance data, which grows from 128 to 160 bytes, so parts with the
same mesh and textures are still one instanced draw, whatever their factors (ADR 0032). Textures
are part of the grouping key. A part without a normal map reads a flat one the renderer makes,
and the shader keeps the mesh's normal where a vertex has no tangent.

The emissive color's `w` carries feature bits: whether the part has a normal map, a
metallic-roughness map, an occlusion map and emission. The shader skips the texture reads and math
of what a part doesn't have. Every pixel of one draw takes the same branches.

### Seven texture slots

`rhi::k_texture_slots` goes from 3 to 7. Slots 3 to 6 sample like slot 0, with linear filtering,
mipmaps and repeating, at Vulkan bindings 4 to 7, DirectX registers t3 to t6 and s3 to s6, and
OpenGL texture units 4 to 7. `rhi::slot_sampler` says which sampler a slot has, and every backend
builds its samplers from it. The forward renderer reads the normal, metallic-roughness, occlusion
and emissive textures at 3 to 6. Seven slots stay within the 16 that OpenGL ES 3 and Vulkan
guarantee per stage.

### Tangents are a module

The new `tangent_generator` interface takes vertices and triangles and returns one tangent per
triangle corner:

- **`mikktspace`** uses MikkTSpace (zlib license), feeding it glTF's texture coordinates with v
  flipped, so the bitangent points up the image as glTF's normal maps expect.
- **`null`** returns none.

`runtime::generate_tangents` asks for tangents for each part whose material has a normal map and
whose vertices have none, and copies vertices that need different tangents in different
triangles. The model loader runs it before making levels of detail. With no generator listed, a
model with a normal map and no tangents logs a warning and draws without its normal map.

### Color spaces

The loader decodes each image once per way it is used: sRGB for base color and emissive, linear
for the rest. Textures are made in the matching format, overriding what a cooked file says, and a
mismatch is logged.

The build's cooking reads the file name: names ending in `_normal`, `_normals` or `_nrm` are cooked
as linear data with each mip level renormalized, and names ending in `_orm`,
`_metallic_roughness`, `_mr`, `_roughness`, `_metallic`, `_occlusion`, `_ao` or `_linear` as linear
data. Everything else stays sRGB.

### glTF's defaults

A material without `metallicFactor` or `roughnessFactor` is fully metallic and fully rough, as
glTF says, and so is a primitive without a material. The samples' models now state both.

## Consequences

- glTF models look as their files describe, on every backend. `docs/guides/materials.md` explains
  how to make and check them.
- The render demo gained a gold ball and a brick wall with normal and ORM maps and no tangents of
  its own, and its reference picture was redone. The other samples' pictures stayed within their
  tolerance once their models stated non-metallic materials.
- Every model's vertices are 16 bytes larger, and every part's instance data 32 bytes larger.
- The shader does more work per pixel. On the Galaxy A52s (Adreno 642L, Vulkan), the render demo's
  GPU time went from 3.87 to 5.31 ms per frame at 1080 x 2400, still within a 120 Hz frame. Most of
  it is the lighting itself; [android-performance.md](../reports/android-performance.md#pbr-materials-2026-11-19)
  has the measurements. Vulkan and OpenGL ES draw the same picture there. Mali and Xclipse phones
  haven't been checked.
- Every sample's Vulkan check runs clean under the Khronos validation layers.
- There is no environment lighting, so smooth metals reflect only the sun and a flat ambient
  color. Image-based lighting would fill `MeshMaterial`'s reflections without changing it.
- Transparency, alpha testing, double-sided materials, a second UV set, `KHR_texture_transform`
  and other material extensions are not read yet.
- Images embedded in a `.glb`, as Blender writes them, are not cooked.
