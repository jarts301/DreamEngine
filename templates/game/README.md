# __GAME_TITLE__

A game made with [DreamEngine](https://github.com/jarts301/DreamEngine), from its project template
(`dream new __GAME_NAME__`).

## Building

The build needs DreamEngine's SDK and the tools the engine's README lists for each platform:
CMake 3.25 or later, Ninja, vcpkg, and Visual Studio 2022 on Windows or the Android NDK for Android.
Two environment variables tell the presets where things are:

| Variable | Names |
|---|---|
| `DREAM_ENGINE_SDK` | The SDK's folder: an installed SDK, or a checkout of the engine |
| `VCPKG_ROOT` | Your vcpkg folder |

On Windows, in a Visual Studio x64 developer prompt:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
build\windows-debug\__GAME_NAME__\__GAME_NAME__.exe
```

The first configure builds the engine's libraries with vcpkg, which takes a while; later ones reuse
them. The engine itself builds inside the game's build folder, with only the modules `dream.toml`
lists.

For Android, set `ANDROID_NDK_HOME` and `ANDROID_HOME` as well, then:

```powershell
cmake --preset android-arm64-debug
cmake --build --preset android-arm64-debug --target __GAME_NAME___apk
```

A PC release ready to hand out is the `__GAME_NAME___package` target of a `windows-release` build.

## What is where

| Path | Holds |
|---|---|
| `src/__GAME_NAME__.cpp` | The game's code |
| `dream.toml` | The window, the controls, and the modules each platform uses |
| `assets/scenes/main.toml` | The first scene |
| `assets/models/` | Models, with their textures in `textures/`, which the build cooks |
| `vcpkg.json` | The engine's libraries. Keep it the same as the SDK's own `vcpkg.json` when you update the SDK |

The engine's guides, in the SDK's `docs/guides/`, cover materials, lighting, sprites, particles,
animation and input.
