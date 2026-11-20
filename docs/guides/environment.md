# Environment lighting and bloom

This guide explains how to light a scene with an HDR image of its surroundings, so metals and glass
reflect the world and surfaces get sky light from above, and how to tune the glow around bright
light. The design is in [ADR 0040](../adr/0040-environment-lighting-and-bloom.md); the sun, point
and spot lights are in the [lighting guide](lighting.md), and surfaces in the
[materials guide](materials.md).

## In short

- An **`environment`** lights the whole scene from an HDR image all around it, in place of the
  sun's flat ambient light. Shiny surfaces reflect it, rough ones blur it, matte ones take its
  color from each side.
- It can be drawn **behind the scene**, as its sky.
- **Bloom** makes light brighter than the screen can show glow: emission, fire, sparks, lights and
  bright highlights. It is on by default, with HDR.

## Adding an environment

```toml
[entities.sky.environment]
map = "assets:/environments/sky.hdr"   # A Radiance .hdr file, equirectangular.
intensity = 0.8                        # All optional but the map.
rotation = 0.0                         # Degrees around +Y, to turn it.
background = true                      # Draw it behind the scene.
```

The world's first `environment` is used. In code it is `runtime::EnvironmentLight`, with an
`EnvironmentAsset` loaded through `app.assets().load<runtime::EnvironmentAsset>(path)` after
`Renderer::initialize`.

Keep the scene's `directional_light`: the environment replaces only its `ambient`, which is then
ignored. The sun still gives the direct light and the shadows.

## Getting environment images

- **HDRI libraries** such as Poly Haven publish free equirectangular `.hdr` images (CC0). Download
  the smallest size, 1K or 2K: the engine reduces it to 256 x 128 anyway, and a large file only
  takes longer to load.
- **Export from Blender** or another renderer: render an equirectangular panorama and save it as
  Radiance HDR.
- **Generate one:** `tools/make_environment.py` writes the render demo's sky: a gradient, clouds,
  a sun and grassland below. Change its colors for other moods.

Images must be **equirectangular** (twice as wide as tall, longitude across) and stored top to
bottom, as these sources save them.

**Line the sun up.** If the image has a sun, turn the environment with `rotation`, or set the
`directional_light`'s direction, so the sun in the image is where the light comes from. Otherwise
shiny surfaces reflect a sun in one place and show the light's highlight in another.

## How bright

The image's values are light in the same units as the sun's intensity: a matte white surface
facing a sky of 0.5 everywhere is lit 0.5 by it. `intensity` multiplies the image.

| Scene | Environment intensity | Sun intensity |
|---|---|---|
| Sunny day | 0.6–1.0 | 1.3–2 |
| Overcast | 1.0–1.5, with a grey sky | 0.3–0.6 |
| Dusk | 0.3–0.5, with a warm sky | 0.5–1, low and orange |
| Night | 0.03–0.1, with a dark blue sky | 0, or a moon at 0.1–0.2 |
| Interior | 0.2–0.4 | Lamps from point and spot lights |

Downloaded HDRIs vary a lot in brightness. Start at 1 and adjust until matte surfaces in shadow
look right: that light comes only from the environment.

## Bloom

```toml
[renderer]
bloom = true              # On by default, with HDR.
bloom_intensity = 0.6     # How strong the glow is, from 0 to 10.
bloom_threshold = 1.0     # Only light brighter than this glows.
```

- **What glows:** anything brighter than the threshold after lighting. White lit by the default sun
  is about 1.3, so with a threshold of 1 the brightest lit surfaces glow a little. Raise the
  threshold to 1.5–2 so only emission, fire and lights glow; lower it below 1 for a dreamy look.
- **Make things glow** by making them brighter than the threshold: an emissive material above 1
  (`emissive = [3.0, 1.2, 0.3]`), particle colors above 1, a bright point light near a surface.
- **Intensity:** 0.3–0.8 looks natural. Above that, everything goes hazy.
- The glow's spread follows the screen's size, so it looks the same at any resolution.

## Phones

Measured in the render demo on a Galaxy A52s (Adreno 642L) at its full 1080 x 2400:

| | GPU time per frame |
|---|---|
| Environment lighting, with the sky drawn behind | 1.2 ms |
| Bloom | 2.0 ms |

- **Environment lighting** costs two extra texture reads per lit pixel, and the sky where nothing
  covers it. Keep it on: it is most of what makes materials look right.
- **Bloom** costs a fixed amount for the screen's size, whatever glows. For weaker phones, or a
  game that needs every millisecond, turn it off with `bloom = false` under `[renderer]`.

## Checking

`samples/render_demo` lights its yard with a generated sky: the gold ball and the red sphere
reflect it, the ground behind the floor is the sky's grassland, and the campfire, lamp and ring of
lights glow.

| You see | Likely cause | Fix |
|---|---|---|
| The scene looks the same as without one | The `environment` failed to load (the log says), or another entity's comes first | Check the path; keep one |
| Everything is too bright or flat | The image is bright, as many HDRIs are | Lower `intensity` |
| Metals reflect a sun where there is no highlight | The image's sun and the light disagree | Turn the environment with `rotation`, or move the light |
| Objects indoors reflect the sky | One environment lights everything | Use a dark interior environment for interior scenes |
| Everything glows | The threshold is below the scene's lit surfaces | Raise `bloom_threshold` |
| Nothing glows | No HDR (phones without float targets), bloom off, or nothing above the threshold | Check the log line that describes the renderer |
| Glowing squares that flicker | A very small, very bright highlight | It is already softened; lower the light or roughen the surface |

## Limits

Each of these is a later step on the roadmap:

- Reflection probes, for interiors and for reflections that change from place to place.
- Reflections of the scene's own objects (screen-space reflections).
- Shadowing of environment light, beyond ambient occlusion maps.
- Preparing environments at build time instead of as they load.
- A sharp sky: the sky behind the scene is drawn from the same 256 x 128 image the lighting uses,
  so it is soft, which suits skies and distant scenery but not a detailed backdrop up close.

## How it works inside

1. **`runtime::EnvironmentAsset`** reads the `.hdr` file, reduces it to 256 x 128, and makes the
   levels the renderer reads: sharp, four of increasing roughness by GGX, and the matte light from
   each side. They go to the GPU as one half-float texture with six levels.
2. **`runtime::Renderer`** gives the first environment to the renderer module as
   `Frame::environment`.
3. **The `forward` module** draws the sky behind the scene with `sky.slang`, and `forward.slang`
   lights each surface from the environment: its matte level toward the normal, and its level for
   the surface's roughness along the reflection.
4. **Bloom**, between the HDR scene and tone mapping, reduces the scene to a quarter keeping its
   brightest light, halves it up to five more times, adds each level back up, and tone mapping
   adds the result to the scene.
