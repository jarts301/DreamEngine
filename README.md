# DreamEngine

A modular C++20 game engine for PC and Android, designed so platforms and subsystems can be swapped
through configuration.

Architecture rules and conventions are in [docs/adr](docs/adr/README.md).

## Requirements

| Tool | Version | Needed for |
|---|---|---|
| CMake | 3.25 or newer | All builds |
| Ninja | Any recent | All builds |
| [vcpkg](https://github.com/microsoft/vcpkg) | Any; `VCPKG_ROOT` must point to it | All builds |
| Visual Studio C++ tools | 2022 or newer | Windows builds |
| [Android NDK](https://developer.android.com/ndk/downloads) | r30; `ANDROID_NDK_HOME` must point to it | Android builds |
| [Android SDK](https://developer.android.com/tools/releases/cmdline-tools) | Platform 35, build-tools 35.0.0; `ANDROID_HOME` must point to it | Android APKs |
| JDK | 17 or newer; `JAVA_HOME` must point to it | Android APKs |

## Build

Windows builds must run from a **x64 Native Tools Command Prompt**. CLion and Visual Studio set this
environment up for you and list the presets automatically.

Build and run the tests on Windows:

```bash
cmake --workflow --preset windows-debug
```

Build for Android (tests are compiled but not run):

```bash
cmake --workflow --preset android-arm64-debug
```

Each preset also has a `-release` version. Output goes to `build/<preset>/`.

## Run the cube

After building, run the cube on Windows. Space or a click toggles the spin, and Escape quits:

```bash
build/windows-debug/samples/cube/dream_cube/dream_cube.exe
```

It draws with Vulkan, or with DirectX on a PC without a Vulkan driver, as its `dream.toml` lists.
To see the same fixed view on each API, run `dream_cube_vulkan_check` and `dream_cube_directx_check`
from their own folders next to it.

To check the Vulkan backend with the Khronos validation layers, configure with
`-DVCPKG_MANIFEST_FEATURES=vulkan-validation` and set `VK_LAYER_PATH` to the folder containing
`VkLayer_khronos_validation.json` under `vcpkg_installed/`. Debug builds then turn validation on.

The DirectX backend is Direct3D 11 and needs nothing installed. Debug builds turn its debug layer on
when Windows' "Graphics Tools" feature is present, and the messages appear in the engine log.

## Play Toppler

Toppler is a small physics puzzle built on the engine: knock every gold crate down within each
level's throws, by tapping or clicking where a ball should land. Escape, or Back on Android, pauses.
After building, run it on Windows:

```bash
build/windows-debug/samples/toppler/dream_toppler/dream_toppler.exe
```

On Android, its APK target is `dream_toppler_apk`. Its levels are scene files in
`samples/toppler/assets/levels/`, and its progress is saved in the engine's user storage. See
[ADR 0020](docs/adr/0020-first-game.md).

## Android APK

The targets are `dream_cube_apk`, `dream_hello_game_apk` and `dream_toppler_apk`, among others.
Build a game's APK, then install it on a phone connected by USB with debugging enabled:

```bash
cmake --build --preset android-arm64-debug --target dream_hello_game_apk
```

```bash
adb install -r build/android-arm64-debug/samples/hello_game/dream_hello_game_apk/app/outputs/apk/debug/app-debug.apk
```

Engine messages appear in Logcat:

```bash
adb logcat DreamEngine:V SDL:V *:S
```

The APK contains only `arm64-v8a` code, so it needs a real device rather than an x86-64 emulator.

## Choosing modules

A project picks its backends in a TOML file, with one priority list per interface and platform.
[samples/module_demo/dream.toml](samples/module_demo/dream.toml) is an example:

```toml
[modules.windows]
greeter = ["english", "spanish"]
```

Only the listed backends are compiled, and the first one that starts is used. To build your own
program with them, call `dream_link_modules(<target> CONFIG <file>)` in CMake and
`core::ModuleProject::load` at startup. See [ADR 0006](docs/adr/0006-module-system.md).

Graphics, physics, audio and the game interface each have two real backends, switched by
editing a list. Each one below runs the arcade sample to the same result on the platforms marked,
checked on a PC and a phone as described in [Backends](docs/reports/backends.md):

| Interface | Backends | Windows | Android | Notes |
|---|---|---|---|---|
| `rhi` | `vulkan`, `directx`, `null` | all | `vulkan`, `null` | A game's shaders are compiled for whichever its list names. |
| `physics3d` | `jolt`, `impulse`, `null` | all | all | `impulse` is the engine's own library. |
| `physics2d` | `box2d`, `chipmunk`, `null` | all | all | `chipmunk` has no continuous collision. |
| `audio` | `miniaudio`, `portaudio`, `null` | all | `miniaudio`, `null` | PortAudio has no Android output. |
| `ui` | `rmlui`, `markup`, `null` | all | all | Both read the same RML and RCSS files. `markup` is the engine's own library. |
| `debug_ui` | `imgui`, `null` | all | all | |
| `platform` | `sdl3`, `null` | all | all | |
| `profiler` | `timeline`, `null` | all | all | `timeline` saves a trace to open in Perfetto; see [Profiling](#profiling). |

```toml
[modules.windows]
rhi = ["directx", "vulkan", "null"]
physics3d = ["impulse"]
ui = ["markup"]
```

Shaders declare their bindings for both graphics APIs; see
[ADR 0013](docs/adr/0013-directx-and-shader-formats.md). The engine's own backends are built on
[libraries](libraries/README.md) of their own; see
[ADR 0014](docs/adr/0014-more-backends-and-libraries.md).

## Profiling

List a profiler for a platform, and the game saves a trace of each run when it ends. Open the file
at [ui.perfetto.dev](https://ui.perfetto.dev) or in Chrome's `chrome://tracing`:

```toml
[modules.windows]
profiler = ["timeline"]

[profiler.timeline]
path = "trace.json"   # Relative to the starting folder; on Android, the app's files folder.
start_frame = 0       # Frames to skip before recording, such as while loading.
frames = 0            # How many frames to record. 0 means until the end.
```

The engine already measures its main loop, physics, rendering, audio, loading and jobs. Games and
backends add their own zones with the macros in `dream/core/profile.h`:

```cpp
#include "dream/core/profile.h"

void Enemies::update() {
    DREAM_PROFILE_ZONE("Enemies");               // Measures the rest of the scope.
    DREAM_PROFILE_VALUE("Enemies alive", count); // Drawn as a graph over time.
}
```

Profiles also record frame time, the main thread's CPU work and the GPU's time per frame, heap
allocations per frame, process memory, CPU usage, video memory and counts such as draw calls and
physics bodies. When the
game ends, the log sums each one up with its minimum, mean, 95th and 99th percentile, and maximum.
[Reading profiles](docs/reports/reading-profiles.md) explains what each value means and how to read
it.

`samples/arcade_demo` saves `dream_arcade_trace.json` this way on Windows and Linux. Configuring
with `-DDREAM_PROFILING=OFF` removes the zones from the build entirely. See
[ADR 0015](docs/adr/0015-profiling.md) and [ADR 0016](docs/adr/0016-profiling-resources.md).

## Running out of memory

Every game watches its memory and the system's. When memory runs low, `Game::on_low_memory` is
called so the game can free some, new assets are refused while it is critically low, and an
emergency reserve lets an allocation that fails succeed, instead of the game crashing. A game can
also give itself a budget:

```toml
[memory]
budget_mb = 2048   # The most private memory the game means to use.
```

[Memory budgets](docs/reports/memory-budgets.md) describes the strategy and what a game should do.
See [ADR 0018](docs/adr/0018-memory-budgets.md).

## Crash reports

When a game or the engine crashes on Windows or Linux, a report is written to `crash_reports/`
with the reason, the frame, the modules in use, memory, the last log lines and the stack, and on
Windows a minidump beside it that Visual Studio opens at the crash. To try it, crash on purpose:

```toml
[crash_report]
simulate = "null_pointer"    # Or "stack_overflow", "abort", "assert", ... "none" turns it off.
simulate_after_frames = 300
```

[Crash reports](docs/reports/crash-reports.md) explains how to read them. See
[ADR 0019](docs/adr/0019-crash-reports.md).

To check the layer rules locally (needs Python 3):

```bash
python tools/check_layers.py
```

## CLion

Use a preset profile, because CLion's default `Debug` profile does not use vcpkg. In
**Settings > Build, Execution, Deployment > CMake**, enable a preset profile and disable `Debug`.

If `VCPKG_ROOT` and `ANDROID_NDK_HOME` are not set on your system, create a `CMakeUserPresets.json`
(ignored by Git) with presets that set them under `environment` and inherit from the presets above.

## Non-English Visual Studio

Ninja finds a source file's headers by matching the compiler's "including file" message. In a
non-English Visual Studio that text can contain accented letters, and their bytes depend on the
console code page. If the console differs between configuring and building, as it can in an IDE,
editing a header silently stops rebuilding the files that use it, which can cause strange crashes.

To avoid this, install Visual Studio's English language pack (**Visual Studio Installer > Modify >
Language packs**). The Windows presets set `VSLANG=1033`, which makes the compiler use it. CLion
doesn't apply that setting when building, so with CLion also remove the other language, leaving
English as the only one. Then reconfigure from scratch and rebuild.

To check a build folder, this should print no lines:

```bash
ninja -C build/windows-debug -t deps | grep "#deps 0,"
```

Without the language pack, rebuild the project after editing a header.
