# 0028. HDR scene and Khronos PBR Neutral tone mapping

- Status: Accepted
- Date: 2026-11-18

## Context

The renderer drew straight into the window's 8-bit sRGB image, so every lit color above 1 was cut
off at full brightness: a bright light turned surfaces flat white, and there was no headroom for
the lighting Phase 12 adds next. Render targets (ADR 0027) make an HDR scene possible on every
backend. Phones pay for each full-screen pass in memory bandwidth, so the format had to suit the
device.

## Decision

`Renderer::render` draws the scene into a float color target and a `Depth32Float` depth target the
size of the window, then draws one triangle over the window that tone-maps it. Debug lines and the
interface layers are drawn after that, onto the window, so their colors are exactly as given.

```toml
[renderer]
hdr = "auto"              # "auto", "r11g11b10", "rgba16f" or "off".
tone_mapping = "neutral"  # "neutral" or "none".
exposure = 1.0            # A factor on the scene's colors, before tone mapping.
```

### The format, per device

`"auto"` takes `Rg11B10Float` where the device can draw into it and filter it, and `Rgba16Float`
where only that works. R11G11B10 is four bytes a pixel against RGBA16's eight, so it halves the
bandwidth of drawing and reading the scene, which matters most on phones, and its precision, about
three significant digits, is plenty for colors the tone mapper then compresses. The scene has no
use for alpha. A device with neither, such as an OpenGL ES 3.1 phone without
`EXT_color_buffer_float`, draws straight to the window as before, with no tone mapping, and the
log says which happened. Both Windows and the phones tried so far take R11G11B10.

### Khronos PBR Neutral

The tone mapper is Khronos PBR Neutral (`runtime/shaders/tonemap.slang`). Below about 0.76 it
leaves colors as the materials have them, except for a small offset that deepens near-black
values; above, it rolls them off smoothly toward white and desaturates the brightest, so a
material's base color appears on screen as authored. Filmic curves such as ACES shift hue and
contrast everywhere, which suits cinematic scenes but not a general engine's default. `"none"`
applies only the exposure and clamps, as without HDR.

The HDR targets follow the window: the renderer makes them again when the size changes, before
anything that frame reads the old ones.

## Consequences

- The renderer's samples look the same, with darker darks from PBR Neutral's offset: the render
  demo's and the arcade's reference images were made again, and match on Vulkan, Direct3D 11 and
  12, OpenGL, OpenGL ES and WARP. Toppler's still match within the tolerance.
- On a Galaxy A57, at 1080x2340, the render demo's GPU time went from 1.45 to 2.47 ms a frame,
  about 1 ms for writing and reading the scene once more, and it still runs at the screen's 120
  FPS. A game that can't afford that sets `hdr = "off"`.
- Games that draw through the graphics interface themselves, such as the cube and scene demos, are
  unchanged.
- Bloom, auto-exposure and a color-grading lookup can come later as more passes over the same
  target.
