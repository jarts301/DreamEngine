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

## CLion

Use a preset profile, because CLion's default `Debug` profile does not use vcpkg. In
**Settings > Build, Execution, Deployment > CMake**, enable a preset profile and disable `Debug`.

If `VCPKG_ROOT` and `ANDROID_NDK_HOME` are not set on your system, create a `CMakeUserPresets.json`
(ignored by Git) with presets that set them under `environment` and inherit from the presets above.
