# 0041. Cascade blending, a frame limit on battery, and fewer allocations per frame

- Status: Accepted
- Date: 2026-11-20

## Context

Three smaller items were left from Phase 12's rendering work and the Android performance report:

- **Cascade seams.** Each cascaded shadow map (ADR 0029) is sharper than the next, so a line showed
  where one cascade ended and the next began, and shadows stopped with a hard edge at the shadow
  distance.
- **Battery.** The game ran at 120 FPS whenever a phone's screen could, costing battery and heat for
  no visible gain in most games. `[app] max_fps` could limit it, but only by sleeping: the screen
  still refreshed at 120 Hz, and there was no way to limit only on battery.
- **Allocations.** The report found about 13 heap allocations every frame in Toppler and the arcade,
  and 6 in the render demo, where a frame should allocate close to nothing.

## Decision

### Blending between shadow cascades

Over the last part of each cascade's range, the forward shader reads the next cascade too and blends
toward it; past the last cascade it blends toward full sunlight, so shadows fade out at the shadow
distance instead of ending. `[renderer.shadows] blend` is how much of each cascade's range blends:
0.1 by default, from 0 (off) to 0.5. The blend costs nine more shadow comparisons, only in that
band. It travels in the w of the frame uniforms' camera direction, which was unused, as the block is
at its 512-byte limit.

### A frame limit on battery, and the display asked to match

`[app] max_fps_on_battery` is a lower limit while the device runs on battery, checked every three
seconds. With a graphics device, whenever the limit changes, and whenever a new surface is attached,
the app asks the display to refresh at the limit's rate; 0 lets it choose again.

Both are optional platform capabilities with do-nothing defaults: `Platform::on_battery` and
`Platform::set_frame_rate_hint`. `sdl3` reads the power state from `SDL_GetPowerInfo`, and on
Android 11 and up asks the window for a refresh rate with `ANativeWindow_setFrameRate`, looked up at
run time, since the engine supports Android 10.

### Allocations per frame

Stacks of every allocation over a few hundred frames, captured on PC, showed where they came from.
The engine's own were fixed by keeping memory from frame to frame:

| Where | What allocated | Now |
|---|---|---|
| Vulkan `begin_render_pass` | A vector of clear values, every pass | An array on the stack |
| Vulkan descriptor sets | A map of the frame's texture combinations, emptied every frame | The map keeps its entries, marked by the pool reset they were made after |
| `World::update_transforms` | A hash set of resolved entities | A list by entity index, marked by update number |
| `forward` light clusters | A copy of the cluster counts | A member, assigned |
| `EventBus::flush` | The queue, swapped for a new one | Two lists swapped back and forth |
| Jolt contacts, `Physics3D::step` | A new list of contacts every step | Lists kept and swapped |
| `UserInterface::update` | A copy of the interface's events | A member, assigned |
| `gather_sprites` | Its sorting lists | Kept from call to call |

What remains is outside the engine's own code: RmlUi allocates as it updates and draws documents,
and the timeline profiler as it records. On the A52s the render demo, with the profiler listed,
went from 6 allocations per frame to 2.

## Consequences

- No visible line between shadow cascades, and shadows fade out at their distance.
- A game can run at 60 FPS on battery and at full speed when plugged in, with the display at 60 Hz
  where it can be. With `max_fps = 60` the A52s held 16.7 ms per frame.
- Frames allocate close to nothing in the engine itself; a game's own allocations, and RmlUi's,
  remain the game's to manage.
- Cooking levels of detail at build time, the last of the smaller items, is still to do: phone
  builds can't run a tool on the build machine yet, which it needs.
