# Materials: PBR, normal maps, transparency and emission

This guide explains how DreamEngine draws surfaces, how to make models whose materials look right
in it, and what to check when they don't. The design is recorded in
[ADR 0033](../adr/0033-pbr-materials-and-normal-maps.md) and
[ADR 0034](../adr/0034-transparency.md).

## In short

- Materials come from your glTF models. The engine reads glTF's **metallic-roughness** materials:
  base color, metallic, roughness, a normal map, ambient occlusion and emission, and their alpha
  mode (opaque, cut-out or blended) and double-sidedness.
- List `tangent_generator = ["mikktspace", "null"]` beside your renderer, so normal maps work on
  models exported without tangents.
- Always set **metallic** and **roughness** on every material. glTF's defaults are *fully metallic*
  and *fully rough*, which makes a plain material look like dull metal.
- Textures that the model refers to as PNG files in a `textures/` folder beside it are compressed
  by the build. Name data maps with a suffix such as `_normal` or `_orm` so they are cooked as
  data, not as colors.
- Normal maps use glTF's convention: green points **up** the image (the "OpenGL" style). Maps made
  for DirectX need their green channel flipped.

## Setting up a project

The `forward` renderer draws materials. Tangents, which normal maps need, come from a module of
their own, as levels of detail do:

```toml
[modules.windows]
platform = ["sdl3"]
rhi = ["vulkan", "directx12", "directx", "opengl"]
renderer = ["forward", "null"]
mesh_simplifier = ["meshoptimizer", "null"]
tangent_generator = ["mikktspace", "null"]

[modules.android]
platform = ["sdl3"]
rhi = ["vulkan", "opengles"]
renderer = ["forward", "null"]
mesh_simplifier = ["meshoptimizer", "null"]
tangent_generator = ["mikktspace", "null"]
```

| `tangent_generator` | What it does |
|---|---|
| `mikktspace` | Works out tangents with MikkTSpace, as glTF asks and as Blender, Substance and most baking tools do, so baked normal maps line up. |
| `null` | Makes none. Models that have their own tangents still use them. |
| *(not listed)* | The same as `null`. A model with a normal map and no tangents then logs a warning, and its normal map is ignored. |

Nothing else needs setting. Materials have no `[renderer]` settings of their own.

## What a material is

Every part of a model has one material, with these properties. Each factor is multiplied by its
texture, and a missing texture counts as white, so the factor alone applies.

| Property | Meaning | Texture |
|---|---|---|
| Base color | The surface's color. For a dielectric, the color of its diffuse light; for a metal, the color of its reflections. Linear RGBA in the file. Its alpha is the opacity, for the cut-out and blended alpha modes. | sRGB color, and alpha |
| Metallic | 0 for wood, stone, plastic, skin, paint and almost everything else; 1 for bare metal. Values between are only for blends, such as the edge of rust. | Blue channel |
| Roughness | 0 is a mirror, 1 is chalk. Most things are between 0.3 and 0.9. | Green channel |
| Normal map | Tilts the surface per pixel, for detail the mesh doesn't have. Its `scale` sets how strongly. | Linear, tangent space |
| Occlusion | Darkens ambient light in creases and corners. Its `strength` blends it in. It never darkens sunlight, which the shadow map handles. | Red channel |
| Emissive | Light the surface gives off, added on top, unaffected by shadows. `KHR_materials_emissive_strength` raises it above 1. | sRGB color |

Metallic, roughness and occlusion can share one texture, as glTF's channels are laid out for
that: **o**cclusion in red, **r**oughness in green, **m**etallic in blue. This "ORM" texture is one
image in memory instead of three.

A `mesh` component's `tint` multiplies the base color, so one white model can be many colors:

```toml
[entities.ball.mesh]
model = "assets:/models/ball.glb"
tint = [0.85, 0.12, 0.10]
```

### Transparency: alpha modes

A material's **alpha mode** says what its base color's alpha, the factor times the texture's
alpha, does. It is glTF's `alphaMode`, which Blender sets from the material's *Blend Mode*
(*Settings* panel, or *Render Method* in newer versions):

| Alpha mode | What it draws | For | Shadow |
|---|---|---|---|
| `OPAQUE` (the default) | Alpha is ignored. | Almost everything | Solid |
| `MASK` | Pixels whose alpha is below `alphaCutoff` (0.5 by default) aren't drawn; the rest are solid. | Leaves, grass, fences, chain-link, cut-out decals | Cut out by the same alpha |
| `BLEND` | Blended over what is behind it, by its alpha. | Glass, water, smoke, ghosts, fading effects | None |

Use **`MASK` whenever the surface is either there or not**, such as a leaf: it costs almost the
same as opaque, sorts correctly, and casts the right shadow. Keep **`BLEND`** for surfaces that
are truly partly see-through:

- A blended surface fades its own diffuse color by its alpha, but keeps its reflections and
  emission, as glass does: a clear pane (alpha 0.1 to 0.3, roughness near 0) still shows the
  sun's highlight.
- Blended parts are drawn after everything else, farthest first, by the middle of each part. Two
  blended parts that overlap or cross can draw in the wrong order, and so can a blended model's
  own faces. Keep blended objects simple and apart: a pane, not a glass bottle with a liquid
  inside.
- They don't hide what is behind them in the depth buffer, and cast no shadow.

**Double-sided** materials (`doubleSided`, Blender's *Backface Culling* off) draw both faces, and
light the back face as the front turned around. Use them for single-sheet surfaces seen from both
sides: leaves, cloth, paper, a fence board. A closed model, such as a crate, doesn't need it, and
drawing it double-sided only costs time.

### Emission

**Emissive** color is light a surface gives off: screens, lamps, lava, glowing runes. It adds to
whatever light the surface reflects, and isn't darkened by shadows or occlusion. In Blender it is
the Principled BSDF's *Emission* color and *Strength*; a strength above 1 is written with
`KHR_materials_emissive_strength`, which the engine reads. The scene is drawn in HDR, so values
above 1 aren't clipped: tone mapping rolls them off toward white, which is why a strong orange
glow looks paler at its brightest. A strength of 1 to 3 keeps a glow's color.

An emissive surface lights only itself: it doesn't light what is around it. Above the bloom
threshold it glows past its edges (see the [environment and bloom guide](environment.md)); to light
its surroundings, put a point light inside it (see the [lighting guide](lighting.md)).

### How light works

There is one sun, a `directional_light`, and ambient light that is the same from every direction:

```toml
[entities.sun.directional_light]
direction = [-0.5, -1.0, -0.6]
color = [1.0, 0.95, 0.85]
intensity = 1.3
ambient = 0.3
```

The units are chosen so that a white, fully rough, non-metal surface facing the sun shows
`color × intensity`, plus `ambient`. Then the HDR image is tone-mapped. Light leaves a surface in
two ways:

- **Diffuse:** the base color, scattered evenly. Metals have none.
- **Specular:** a reflection of the sun, a sharp highlight on smooth surfaces and a broad sheen on
  rough ones. It is 4% of the light for dielectrics seen straight on, more at grazing angles, and
  the base color for metals.

Ambient light gives both a diffuse part and an even specular part, which occlusion darkens.

There is no environment map yet: nothing reflects the sky or the scene around it. Smooth metals
therefore reflect only the sun's highlight and a flat ambient color, and look darker than they
would in a tool that shows an environment. That is expected. See [Limits](#limits).

## Making models

### Exporting from Blender

Blender's Principled BSDF maps onto glTF directly. In the shader:

- **Base Color**: an Image Texture, color space *sRGB*, or a plain color.
- **Metallic** and **Roughness**: values, or a texture. A packed ORM texture goes through a
  *Separate Color* node: green into Roughness, blue into Metallic. Its color space must be
  *Non-Color*.
- **Normal**: Image Texture (color space *Non-Color*) → *Normal Map* node (tangent space) → the
  BSDF's Normal input.
- **Emission**: a color and strength, or an Image Texture.
- Ambient occlusion has no Principled input. The glTF exporter reads it from a *glTF Material
  Output* node group, which the exporter's documentation describes.

Then *File → Export → glTF 2.0* with:

| Setting | Value | Why |
|---|---|---|
| Format | **glTF Binary (.glb)** | The engine reads `.glb`. |
| Mesh → Apply Modifiers | On | The engine reads meshes as they are. |
| Mesh → Normals | **On** | Required: models without normals are refused. |
| Mesh → Tangents | On, or off with `mikktspace` listed | Either way the tangents are MikkTSpace's, as Blender's own are. |
| Mesh → UVs | On | Every texture uses the first UV map. |
| Transform → +Y Up | On | The engine's Y axis points up. |

Blender's `.glb` holds its images inside the file. They work, with the right color space for each
use, but they load as PNG or JPEG, uncompressed on the GPU at 4 bytes per pixel, and the build
can't cook them. That is fine for trying a model and costly for shipping many. The build cooks
images that a `.glb` refers to as files in a `textures/` folder beside it, as the samples' models
do (`tools/make_sample_models.py` writes them). Reading `.gltf` files, whose images are always
separate, is on the roadmap.

### Textures

**Name data textures by what they hold.** The build cooks every PNG in a `textures/` folder into
compressed KTX2 (ADR 0024), and the name decides how its smaller mip levels are made:

| Name ends in | Cooked as | Use for |
|---|---|---|
| `_normal`, `_normals`, `_nrm` | Linear data, renormalized at every level | Normal maps |
| `_orm`, `_metallic_roughness`, `_mr`, `_roughness`, `_metallic`, `_occlusion`, `_ao`, `_linear` | Linear data | Metallic, roughness and occlusion maps |
| anything else | sRGB color | Base color and emissive maps |

The case doesn't matter: `Rock_Normal.png` counts. The engine always reads a texture the way its
material uses it. A data map cooked as a color still works, but its smaller levels are made with
the wrong curve, which flattens normal maps at a distance and shifts roughness, and the engine
logs a warning naming it.

**Normal maps** must be tangent-space maps in glTF's convention: red is +X along the U direction,
green is +Y *up the image*, blue points out of the surface, and a flat area is (128, 128, 255).
This is the "OpenGL" convention of Substance, Blender and most bakers. A map made in the
"DirectX" convention, where green points down, makes bumps look like dents when the light comes
from above; flip its green channel before exporting.

**Sizes:** whole multiples of 4 on each side, so they compress (BC7 on PC, ASTC on phones), and
powers of two, so every mip level stays a whole number of blocks. 512, 1024 and 2048 are typical;
1024 is plenty for most props on a phone.

**Keep textures shared.** Parts are drawn together, as instances of one draw, when they have the
same mesh and the same textures. Their factors and tint can differ freely. Many variants of one
prop should share textures and differ in their factors or tint, not in copies of the same image.

### Values for common materials

Base colors here are linear, as glTF stores them, which a tool's color picker converts to.

| Material | Base color | Metallic | Roughness |
|---|---|---|---|
| Fresh snow | 0.85, 0.85, 0.85 | 0 | 0.6–0.8 |
| Concrete, plaster | 0.35–0.55 gray | 0 | 0.8–0.95 |
| Brick | 0.40, 0.12, 0.07 | 0 | 0.85–0.95 |
| Wood, unvarnished | 0.25, 0.14, 0.07 | 0 | 0.7–0.9 |
| Glossy plastic | any | 0 | 0.2–0.4 |
| Rubber | 0.02–0.05 | 0 | 0.8–0.95 |
| Iron, steel | 0.53–0.56 gray | 1 | 0.2–0.6 |
| Aluminium | 0.91, 0.92, 0.92 | 1 | 0.2–0.5 |
| Copper | 0.95, 0.64, 0.54 | 1 | 0.2–0.5 |
| Gold | 1.00, 0.71, 0.29 | 1 | 0.1–0.4 |

Dielectric base colors stay between about 0.02 (charcoal) and 0.9 (snow). Below that, surfaces
look like holes; above it, they glow under bright light. Metals are bright: a dark metal looks
like plastic.

## Checking a model

`samples/render_demo` has a gold ball, a metal; a brick wall with a base color, a normal map and
an ORM texture and no tangents of its own; a blended glass pane; a fence cut out of a board by its
texture's alpha, and double-sided; and a glowing lamp. `tools/make_sample_models.py` makes them. Put a model
beside them in `samples/render_demo/assets/scenes/yard.toml` to see it under the same light.
Hot reload picks up a re-exported model while the game runs.

| You see | Likely cause | Fix |
|---|---|---|
| Everything looks dull, dark or like colored metal | The material doesn't state metallic, so glTF's default of 1 applies | Set Metallic to 0 in Blender, or `metallicFactor: 0` |
| A metal looks dark brown, not shiny | No environment to reflect (see [Limits](#limits)) | Lower its roughness so the sun's highlight shows, and keep its base color bright |
| Bumps look like dents | A DirectX-style normal map | Flip the green channel |
| The log says a model "has normal maps but no tangents" | No `tangent_generator` listed, and none in the file | List `tangent_generator = ["mikktspace", "null"]`, or export tangents |
| The log says a texture "was cooked as colors, but a material reads it as linear data" | A data map's name has no data suffix | Rename it, such as `wall_normal.png` |
| Normal map detail fades out with distance more than it should | The same | The same |
| Seams along UV edges on a normal-mapped model | Tangents made by a different method than the bake used | Export the model's own tangents, from the tool that baked the map |
| A texture is misplaced, with a warning about texture coordinates | The material uses a second UV map, and the engine reads the first | Bake or move it onto the first UV map |
| Glowing parts don't light their surroundings | Emissive is only the surface's own color | Put a point light inside; above the bloom threshold it glows |
| A glow looks white rather than its color | Its emission is far above 1, and tone mapping rolls it off toward white | Lower the strength to 1–3 |
| A see-through texture draws black or white where it should be clear | The material is `OPAQUE` | Set its alpha mode to `MASK` (cut-out) or `BLEND` |
| Leaves or a fence vanish from one side | Back faces are culled | Make the material double-sided |
| Glass hides objects behind it, or two glass objects swap places as the camera moves | Blended parts sort per part, and crossing ones can't be ordered | Keep blended objects apart, or split one into parts |
| Cut-out edges look jagged or thin out in the distance | Alpha testing has no antialiasing, and smaller mip levels average alpha down | Lower `alphaCutoff` a little, or give the texture thicker shapes |
| A material looks right in Blender's viewport but not here | Blender lights it with an environment | Compare in Blender with one sun and the World's strength at 0, plus a little ambient |

## Performance

Materials cost GPU time on every pixel they cover. On a Galaxy A52s, a 2021 mid-range phone, the
render demo's lit yard takes 5.3 ms of GPU time per frame at 1080 x 2400, against 3.9 ms with the
old fixed lighting: comfortably inside a 120 Hz frame of 8.3 ms
([measurements](../reports/android-performance.md#pbr-materials-2026-11-19)). To keep it low:

- **Leave out textures a material doesn't need.** The renderer skips the normal,
  metallic-roughness, occlusion and emissive reads of parts that have none, so a prop with only a
  base color costs less than one with a full set. A factor alone, such as `roughnessFactor: 0.6`,
  is free.
- **Pack occlusion, roughness and metallic into one ORM texture.** It is one image in memory and
  one draw group instead of three textures.
- **Share textures between props.** Parts with the same mesh and textures are drawn as one
  instanced draw, whatever their factors and tint.
- **Size textures for the screen.** 1024 x 1024 is plenty for most props on a phone, and cooked
  textures are compressed and mipmapped, so distant parts read small levels.
- **Mind what covers the screen.** The cost is per pixel: a floor, a wall or a terrain that fills
  the view matters far more than a small detailed prop.

## Limits

These are not supported yet. Each is a later step on the roadmap.

- **Reflection probes**: one environment lights the whole scene (see the
  [environment guide](environment.md)).
- **Order-independent transparency**: blended parts sort per part, so crossing ones can blend in
  the wrong order.
- **Shadows from blended parts**, and colored shadows through glass.
- **Antialiased cut-outs** (alpha to coverage).
- **glTF extensions** besides `KHR_texture_basisu` and `KHR_materials_emissive_strength`, such as
  clearcoat, transmission, sheen and `KHR_texture_transform`.
- **A second UV map**: every texture uses the first.
- **`.gltf` files**, and cooking images embedded in a `.glb`.
- **Materials set from code or scene files**: only the tint. Materials come from the model.

## How it works inside

For engine developers, the path from file to pixel:

1. **`runtime::parse_glb`** reads each material's factors and textures into `ModelMaterialData`.
   Each image is decoded once per way it is used (`ImageData::color`), and tangents in the file are
   moved into the model's space, flipping their sign where a node mirrors the model.
2. **`runtime::generate_tangents`** asks the `tangent_generator` module for one tangent per
   triangle corner, for each part with a normal map and no tangents, and copies vertices where
   neighbouring triangles need different tangents, as on mirrored UVs.
3. **`runtime::generate_lods`** then makes the simpler versions over those vertices (ADR 0032).
4. **`runtime::create_model_asset`** creates the textures in sRGB or linear formats by use, and
   turns each material into a `renderer::MeshMaterial`.
5. **`runtime::Renderer`** puts the material, tinted, into each `renderer::MeshDraw`.
6. **The `forward` module** groups parts by mesh and textures, and sends each part's factors as
   per-instance data. `forward.slang` lights each pixel with GGX, Smith's height-correlated
   visibility and Schlick's Fresnel for the sun, and an analytic fit for ambient specular, then
   adds emission. It draws opaque parts, then cut-out ones with `forward_masked.slang`, which adds
   the alpha test, then blended ones farthest first, each with a one-sided and a double-sided
   pipeline. Cut-out parts cast shadows through `shadow_masked.slang`.

`renderer::MeshVertex` is 48 bytes: position, normal, UV and a tangent whose `w` is the
bitangent's sign, all zero where there is no tangent. The graphics interface has seven texture
slots; materials use these:

| Slot | Texture | Read as |
|---|---|---|
| 0 | Base color | sRGB |
| 3 | Normal | linear; a flat 1×1 texture when the material has none |
| 4 | Metallic-roughness | linear |
| 5 | Occlusion | linear |
| 6 | Emissive | sRGB |

Slot 1 is the tone-mapping pass's input and slot 2 the shadow map (ADR 0027). Another renderer
module receives the same `MeshMaterial`s and may shade them however it likes; `null` ignores them.
