# 0040. Environment lighting and bloom

- Status: Accepted
- Date: 2026-11-20

## Context

Besides the sun and point and spot lights, the scene was lit by a flat ambient light, the same from
every side and white. Shiny metals and glass had nothing to reflect but that flat light and the
sun's highlight, so they looked dull or plastic, and outdoor scenes lacked the blue of the sky above
and the darker ground below. Light brighter than the screen can show, such as emission, fire, sparks
and the sun's highlights, was only clipped to white by tone mapping, without the glow that makes it
read as bright.

The graphics interface has no compute shaders and no cube maps. It had seven texture slots, all
used, and half-float textures only as render targets.

## Decision

### Environment lighting: an equirectangular image with prefiltered levels

The environment is an HDR image of the surroundings in equirectangular form: longitude across,
latitude down, +Y up. Equirectangular rather than a cube map, because the graphics interface has no
cube maps, and one 2D texture with mip levels works on every backend.

`runtime::EnvironmentAsset` loads a Radiance `.hdr` file, the format HDRI libraries publish, with
the engine's own reader, and prepares it on the CPU as it loads, in a few tens of milliseconds:

| Level | Size | What it holds |
|---|---|---|
| 0 | 256 x 128 | The image, area-averaged |
| 1 to 4 | 128 x 64 to 16 x 8 | The image as surfaces of roughness 0.25, 0.5, 0.75 and 1 reflect it: GGX importance sampling, 64 samples per texel, each reading the image's mip pyramid at the level its sample covers |
| 5 | 8 x 4 | What a matte surface receives from each side: the cosine-weighted average over the sphere, divided by pi |

Diffuse light is read from the last level toward the normal; reflections from the level for the
surface's roughness, along the reflected view, with the same split-sum approximation the flat
ambient light used (Karis's fit). Keeping the matte light in the same texture as its last level
costs no uniforms or second texture, and keeps the frame's uniform block at its 512-byte limit.
Without an environment the flat ambient light is used as before, so scenes that don't have one look
the same.

The texture is half floats (Rgba16Float), which the graphics interface now accepts as a filled,
filtered texture as well as a render target, on every backend. It is read at a new texture slot,
7, with a linear, mipmapped, repeating sampler: binding 9 in Vulkan, since binding 8 was already the
per-draw block's, `t7` and `s7` in DirectX, and texture unit 9 in OpenGL. `rhi::texture_binding`
maps slots to bindings.

The environment can be drawn behind the scene, in place of the clear color: `sky.slang`, one
triangle over the window, reading level 0 along each pixel's view direction. It is drawn after
opaque surfaces, at the far end of depth, so only pixels that nothing covers are shaded.

Scene files write it as an `environment` component with a map, an intensity, a rotation around +Y,
and whether it is drawn behind the scene. The world's first one is used.

### Bloom: halving and adding back

With HDR, after the scene's pass and before tone mapping, `forward` and `forward_plus` add a glow
around light above a threshold:

1. The HDR target is reduced to a quarter of its size, then halved up to five more times, down to
   about 16 pixels, with Jimenez's 13-tap filter (`bloom_down.slang`). The first halving keeps only light above the threshold, with a soft knee,
   and weights each group of taps down by its brightness, so a single very bright pixel can't make
   a flashing square.
2. Each smaller image, blurred with a 3 x 3 tent (`bloom_up.slang`), is added to the next larger.
   Adding uses the premultiplied blend with an alpha of 0, so no new blend mode was needed.
3. Tone mapping reads the largest, a quarter of the scene's size, at texture slot 3, with one
   filtered read, and adds it, scaled by the intensity over the number of levels.

Starting at half the scene's size, and adding the glow back in a pass of its own over the whole HDR
target, bloom cost the A52s 3.7 ms per frame at 1080 x 2400; starting at a quarter and adding it in
tone mapping with one read, 2.0 ms.

Settings: `renderer.bloom` (on by default), `renderer.bloom_intensity` (0.6) and
`renderer.bloom_threshold` (1.0, in the scene's linear units, where white lit by the default sun is
about 1.3). Without HDR there is no bloom.

## Consequences

- Metals and glass reflect their surroundings, and outdoor scenes get sky light from above and
  darker light from below. The render demo's yard has a generated sky, `tools/make_environment.py`.
- Emission, fire, sparks, lights and bright highlights glow.
- One environment per scene: no reflection probes for interiors, which would reflect the sky
  indoors. Placed probes, blended between, are a later step.
- Reflections don't see the scene's own objects, and the environment isn't shadowed: a surface
  under a roof still receives the sky's light. Ambient occlusion maps help.
- Loading an environment costs a few tens of milliseconds of CPU time per map. Prefiltering at
  build time is a later step.
- On the A52s at 1080 x 2400, environment lighting costs 1.2 ms of GPU time per frame and bloom
  2.0 ms, in the render demo. Bloom can be turned off in the project's settings.
