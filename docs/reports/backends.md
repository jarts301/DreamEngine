# Backends

Phase 10 asked for the arcade sample to run with every backend of every interface, changing only
its `dream.toml`, on Windows and on Android wherever a backend exists. This page records which
backends work where, and how that was checked.

## Which backends work where

| Interface | Windows | Android | Why not |
|---|---|---|---|
| `rhi` | `vulkan`, `directx`, `null` | `vulkan`, `null` | Direct3D exists only on Windows. |
| `physics3d` | `jolt`, `impulse`, `null` | `jolt`, `impulse`, `null` | |
| `physics2d` | `box2d`, `chipmunk`, `null` | `box2d`, `chipmunk`, `null` | |
| `audio` | `miniaudio`, `portaudio`, `null` | `miniaudio`, `null` | PortAudio 19.7 has no Android output. |
| `ui` | `rmlui`, `markup`, `null` | `rmlui`, `markup`, `null` | |
| `debug_ui` | `imgui`, `null` | `imgui`, `null` | |
| `platform` | `sdl3`, `null` | `sdl3`, `null` | |
| `profiler` | `timeline`, `null` | `timeline`, `null` | |

The samples' own lists are unchanged: the Windows lists name the second backends, and the Android
lists use the first ones. Any backend in the table can be listed instead.

On Android, the `timeline` profiler saves its trace in the app's files folder, which is the game's
working directory there. A debug build's trace can be copied off the phone with:

```sh
adb exec-out run-as org.dreamengine.arcade cat files/dream_arcade_trace.json > trace.json
```

An Android trace has the same values as a Windows one, except the heap counts, which Android
builds leave out, and the memory budget values, as memory watching is off there by default.

## How it was checked

The arcade sample's scripted run throws three balls, opens the menu, turns the music down and
resumes. It ends by logging its result, such as
`score=3 hits_heard=13 menu_opened=1 music_volume=0.25 debug_drawn=yes ui_commands=some`, and
the log shows which backend each interface used. Each set of backends below was put in
`samples/arcade_demo/dream.toml`, with `scripted = true` under `[demo]`, and nothing else changed.

On Windows, a debug build with MSVC 14.38, on an RTX 5080:

| rhi | physics3d | audio | ui | debug_ui | profiler |
|---|---|---|---|---|---|
| vulkan | jolt | miniaudio | rmlui | imgui | timeline |
| directx | impulse | portaudio | markup | imgui | timeline |
| vulkan | impulse | portaudio | markup | null | null |
| directx | jolt | miniaudio | rmlui | null | null |
| null | null | null | null | null | null |

On a Samsung Galaxy A57 (Android 17, Xclipse 550), a debug build:

| rhi | physics3d | audio | ui | debug_ui | profiler |
|---|---|---|---|---|---|
| vulkan | jolt | miniaudio | rmlui | imgui | null |
| vulkan | impulse | miniaudio | markup | imgui | null |
| vulkan | impulse | null | markup | null | null |
| vulkan | jolt | miniaudio | rmlui | null | null |
| vulkan | impulse | miniaudio | markup | imgui | timeline |

Every set with real physics scored 2 or 3 of 3 throws. The score depends on real frame timing, and
the sample's checks accept any score from 1. The null set scores 0, as it has no physics. The
physics demo's scripted run passed on the phone with `impulse` and `chipmunk`.

### Leaks

Each set was also run with the script repeated: Restart, then the throws and the menu, 30 times on
Windows and 10 times on the phone. Both graphics backends now log a warning when the device closes
while textures, buffers, shaders or pipelines the game created are still alive, and none was
logged. The Vulkan backend also frees such leftovers, which it didn't before.

With the profiler off, the game's private memory stayed flat over the 30 rounds on Windows: about
291 MB with Vulkan, and 147 MB with Direct3D. With the `timeline` profiler on, the heap grows about
1.5 KB per frame. That is the trace itself, kept in memory until the game ends and capped by
`max_events`, not a leak.

## What wasn't checked

- Release builds of the second backends on the phone, and any Android phone other than this one.
- Vulkan validation on these runs, since this build had no validation layers.
- Linux, which has no build machine or CI job.
