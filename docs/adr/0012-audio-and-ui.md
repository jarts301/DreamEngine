# 0012. Audio and interfaces

- Status: Accepted
- Date: 2026-11-09

## Decision

### Audio

The `audio` interface plays sounds through voices, each on one of three buses (effects, music,
interface) whose volumes can be set as a whole. A voice can be positioned in the world, heard
relative to a listener. Sounds are WAV, MP3 or FLAC files: short ones are decoded once, and long ones
are streamed, decoded while they play.

The miniaudio backend can mix without sound hardware (`[audio.miniaudio] device = false`), so its
tests check the actual mixed samples, and the scripted sample check runs on machines with no sound
card. A null backend plays nothing.

`runtime::Audio` loads sounds through the AssetManager (files over 256 KB stream), adds the scene
components `audio_source` and `audio_listener`, and stops output while the program is in the
background.

### Two interface layers, one way of drawing

As planned in ADR 0001, there are separate interfaces for the game's interface (`ui`, with RmlUi)
and for developer tools (`debug_ui`, with Dear ImGui), because they have different needs.

Neither backend touches the graphics API. Both describe their triangles in a shared
`ui::DrawList` (pixel positions, sRGB vertex colors, textures by id, clip rectangles), and
`runtime::Renderer` draws the lists as layers over the scene: the game's interface, then the
debug tools. Backends get files and textures through `ui::Host`, which the renderer provides, so
they work with any graphics backend, including future ones.

The debug interface exposes a small set of widgets rather than ImGui itself, since games may not
include backend headers (ADR 0001). That is enough for debug panels, and any immediate-mode
library can provide it.

### Graphics interface additions

- Blending: alpha, and premultiplied alpha, which RmlUi uses.
- Scissor rectangles.
- `bind_frame_geometry`, which copies vertices and 32-bit indices into per-frame memory.
- Four-byte normalized vertex attributes, for colors.

Interface colors are sRGB. The UI shader converts them to linear, like every other shader output,
so the window's sRGB format encodes them correctly.

### Platform additions

Text input (`TextEntered`, plus `set_text_input`, which shows the on-screen keyboard), the mouse
wheel, the Delete, Home and End keys, and `display_scale`. Both interfaces scale with the display,
so they stay readable on phones.

### Lifetime rules

RmlUi's state is global, so only one RmlUi interface may exist at a time. Textures released by an
interface are destroyed at the next update, never while a frame is being recorded. Interfaces must
be destroyed before the renderer that provides their textures.

## Consequences

- RmlUi's transforms, clip masks, filters and shaders are not supported yet; elements using them
  draw untransformed or unfiltered.
- ImGui's texture updates replace the whole texture rather than the changed parts.
- Debug UI offers only the widgets above.
- The sample fonts are Lato, under the SIL Open Font License, which travels with them. Sample
  sounds are synthesized by `tools/make_sample_sounds.py`.
