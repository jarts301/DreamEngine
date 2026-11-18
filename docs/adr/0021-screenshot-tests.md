# 0021. Screenshot tests

- Status: Accepted
- Date: 2026-11-17

## Context

The engine draws with Vulkan and Direct3D 11, and its games' interfaces with RmlUi or markup. Every
check so far proved that frames were drawn and that the game behaved, but not what the frames
looked like: a backend could draw the wrong colors, a shader could break, or a layout could change,
and the checks would still pass. Comparing pictures by eye, as in earlier stages, doesn't repeat
itself.

## Decision

**Devices can capture a frame.** `rhi::Device` gains an optional capability:
`capture_next_frame()` asks for the next frame that is drawn, and `take_captured_frame()` hands it
over once presented, as RGBA rows from the top, with colors as the screen shows them. Vulkan copies
the swapchain image into a readable buffer, where the surface allows copying from it, and Direct3D
copies the back buffer into a staging texture. Both wait for the GPU, which is fine for tests. The
null device can't capture.

**Games are checked by configuration.** A `[screenshot]` table makes the App capture one frame,
save it as PNG next to the program, and compare it with a reference image, failing the run if more
than `max_differing` of its pixels are off by more than `tolerance` in any color channel. Alpha is
ignored. While the table is set, each frame advances the game by exactly one fixed step, whatever
the clock says, so a frame number names the same moment of the game on every machine.

**References are images in the source tree**, in each sample's `tests/reference/screenshots/`,
copied in as assets of the check programs. To make or update one, run the check, look at the
capture it saves, and copy it there.

**Where they run:**

| Program | Graphics | Runs |
|---|---|---|
| `<sample>_vulkan_check` | Vulkan, on the GPU | By hand |
| `<sample>_directx_check`, `<sample>_directx12_check` | Direct3D 11 or 12, on the GPU | By hand, Windows |
| `<sample>_warp_check`, `<sample>_directx12_warp_check` | Direct3D 11 or 12 on WARP, Windows' software rasterizer | CTest, Windows, so CI too |

The cube, the render demo, the arcade and Toppler each have one reference. The GPU checks still
play their scripted runs to the end after comparing. Toppler's DirectX check draws its interface
with markup, which places text a little differently from RmlUi, so it has a reference of its own.

On Android, where the screen sets the size, a capture of another size than the reference is saved
but not compared.

## Consequences

- On an RTX 5080, Vulkan and Direct3D drew the four references identically, and WARP was within the
  tolerance, with fewer than 0.001% of pixels over it. A tolerance of 24 and 1% of pixels leave room
  for other GPUs and drivers.
- The first run already found a bug: with markup, Toppler's level select fit four levels a row
  instead of five, because markup, like a web page, gives the whitespace between the cells room.
- A change that is meant to alter a picture needs its reference updated in the same change.
- Phones aren't compared, since their screens differ. Captures from them are saved, to look at.
