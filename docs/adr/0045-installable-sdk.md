# 0045. An installable SDK, `dream new`, and games in their own repositories

- Status: Accepted
- Date: 2026-11-23

## Context

Until now every game was a folder in the engine's repository, added to `samples/CMakeLists.txt`.
A studio's game should live in a repository of its own, with the engine as something it uses, not
something it is inside. Phase 13 of the roadmap asks for three things:

- a CMake package, so a game's build says `find_package(DreamEngine)`;
- `dream new <name>`, which makes a project from a template;
- games in their own repositories.

The engine is built differently from most libraries. A game's `dream.toml` picks its modules for
each platform, and the build compiles only those, with the game's shaders for the graphics backends
it lists, and cooks the game's assets with tools that run on the build machine. The engine's
third-party libraries come from vcpkg, for each target: Windows, and Android cross-compiled.

There were two ways to make an SDK:

| | Binary SDK: prebuilt libraries | Source SDK: the engine's sources, built with the game |
|---|---|---|
| A game's first build | Fast: only the game compiles | Slower: the engine's core and the game's modules compile once, in the game's build folder |
| What it holds | Every module, for every platform, build type and compiler, with every third-party library they link | One copy of the sources, for every platform |
| Module choice | All modules prebuilt, so the per-game choice only picks among them | As inside the engine: only listed modules compile |
| Compiler and settings | Must match the SDK's exactly, or the libraries don't link | The game's own |
| Android | A second set of libraries | The same sources, cross-compiled by the game's preset |

## Decision

### The SDK is the engine's sources

`cmake --install <build> --prefix <folder>` installs the SDK: the engine's sources, its CMake
scripts and cookers, its vcpkg overlay ports and triplets, the Android template, the docs, and a
`bin` folder with the `dream` tool. A checkout of the engine is an SDK too, with the same layout;
only `bin/dream` is missing, and the tool built in the checkout finds the template there.

`cmake/DreamEngineConfig.cmake` is the package. `find_package(DreamEngine 0.1 REQUIRED)` adds the
engine's sources to the game's build, in `<build>/dream_engine`, excluded from `all` so only what
the game uses is built, and gives the game `dream_add_game` as the samples have it. The engine's
tests and samples stay off, and its install rules too, so a game's own `cmake --install` doesn't
copy the engine. `DreamEngineConfigVersion.cmake` reads the version from the engine's
`CMakeLists.txt`: while it is 0.x only the same minor version matches, since each may break games.

The engine's CMake functions already used cache variables for what they shared, with two
exceptions, the texture suffix lists, which moved inside the function that uses them.

### `dream new`

`dream` is a small C++ program, built on the build machine like `dream_pack`:

```
dream new <name> [--path <folder>] [--id <application id>] [--title <title>]
dream version
```

`new` copies `templates/game` from its SDK, the folder above its `bin`, filling in the game's name,
title, application ID and the engine's version. It writes the game's `vcpkg.json` from the SDK's
own, renamed and without the engine's test framework, so the two never drift apart. The project
has:

| File | Holds |
|---|---|
| `CMakeLists.txt` | `find_package(DreamEngine)` and `dream_add_game` |
| `CMakePresets.json` | The engine's four presets, finding the SDK through `DREAM_ENGINE_SDK` and vcpkg through `VCPKG_ROOT` |
| `vcpkg.json` | The engine's libraries, at the engine's baseline |
| `dream.toml` | A window, two actions, and module lists for Windows, Android and Linux |
| `src/<name>.cpp` | A game that loads a scene and spins what has its own `spin` component |
| `assets/` | The scene, a crate and a ground with their textures, which the build cooks |
| `.gitignore`, `README.md` | Ready for `git init` |

The SDK's location is an environment variable, like `VCPKG_ROOT`, so the project holds no path of
one developer's machine and each developer can keep the SDK anywhere.

### Tested

`dream_new`, a CTest test, makes a project and checks every file, that no placeholder is left, that
the manifest was made from the SDK's, that the template's version matches the engine, and that bad
names, IDs and full folders are refused. The template's own code, configuration and assets also
build inside the engine as `samples/template`, placeholders and all, and `dream_template_null` runs
its scene for a few frames on the `null` backends, so an engine change that breaks new games fails
the engine's tests.

Before this change landed, an SDK installed from a Windows build made a game outside the engine's
repository, which was configured and built with its own presets, ran on the GPU with its cooked
textures, and built a release APK for Android.

## Consequences

- A game is a repository of its own, a few files and its assets, and updates the engine by
  pointing `DREAM_ENGINE_SDK` at a newer SDK.
- Each game's build folder builds the engine's core and the game's modules once, as the samples
  do; vcpkg's binary cache makes its libraries quick after the first game.
- On Windows, the engine builds deep in the game's build folder, under `dream_engine`, so a game
  should live at a short path: paths over 250 characters fail to compile.
- A game's `vcpkg.json` must follow the SDK's when the SDK is updated. `dream new` writes it, but
  nothing yet updates an existing game's.
- Still to do: a binary SDK, if first builds become too slow; `dream` commands beyond `new`, such
  as adding a module to a project or updating its manifest; and a Linux check of a game outside the
  engine.
