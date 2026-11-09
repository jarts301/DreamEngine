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

## Android APK

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

To check the layer rules locally (needs Python 3):

```bash
python tools/check_layers.py
```

## CLion

Use a preset profile, because CLion's default `Debug` profile does not use vcpkg. In
**Settings > Build, Execution, Deployment > CMake**, enable a preset profile and disable `Debug`.

If `VCPKG_ROOT` and `ANDROID_NDK_HOME` are not set on your system, create a `CMakeUserPresets.json`
(ignored by Git) with presets that set them under `environment` and inherit from the presets above.
