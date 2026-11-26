# 0047. The asset database, and bundles through the asset_source interface

- Status: Accepted
- Date: 2026-11-25

## Context

Phase 13 of the roadmap asks for an asset database: dependencies between assets, cooking per
platform, and bundles, which section 3.1 places behind an `asset_source` interface. Until now:

- The build staged every file of a game's asset folders and cooked some of them, without knowing
  what any file uses. A scene naming a model that doesn't exist, after a typo or a rename, built
  fine and failed when the game loaded it.
- A PC game's package was one `assets.pak`, read by the `sdl3` platform. A game couldn't ship
  part of its assets apart, such as one level's, to load and release together, or to replace in a
  patch.
- Nothing recorded which cooker made a game's textures for which platform.

The database must work for Android builds too, which cross-compile and can't run tools built for
the phone; the model cook tool needed a vcpkg port for that (ADR 0042).

## Decision

### The asset database, made at build time

After staging a game's assets, the build runs `cmake/DreamAssetDatabase.cmake`, a CMake script,
so it needs no tool built for the build machine and runs the same for every platform. It writes
`asset_database.toml` among the staged assets. For each file it records:

| Field | Holds |
|---|---|
| `paths` | Its path under `assets:/` |
| `kinds` | `scene`, `model`, `texture`, `environment`, `sound`, `font`, `shader`, `ui`, `style`, `lods`, `data` for other TOML files, or `file` |
| `bytes` | Its size |
| `needs` | The files it names, joined with `|` |
| `bundles` | The bundle it ships in, or `base` |

At the top are the platform and the texture cooker the build used, so the database describes the
assets as cooked for one platform.

What a file needs is what it names:

- **TOML files:** every `"assets:/..."` string, such as a scene's models and sounds. A path without
  its ending, as a shader pipeline names its shaders, names each file that starts with it and a dot.
- **Models:** each image their glTF names by file, beside them, and their cooked `.lods`.
- **RML and RCSS:** the files their `href`, `src` and `url()` name, beside them.

A PNG the build cooked is needed as its `.ktx2`, the file the game reads.

**A file that names one the assets don't have fails the build**, naming both, with every such
problem listed at once. Files the game's code loads by name aren't scanned; only data files are.

`core::AssetDatabase` reads the database: each file's kind, size, needs and bundle, everything a
file needs through what it needs (`needs_all`), and the files that need one (`needed_by`). The App
reads it at start, through the platform, as `App::asset_database()`, empty for a game without one.

### Bundles

A project names bundles in its `dream.toml`, each by the files it starts from:

```toml
[bundles.forest]
roots = ["assets:/scenes/forest.toml"]
```

A bundle holds its roots and everything they need, except what another bundle needs too, which
stays in the base. The base, which also holds everything no bundle needs, is always there.

On PC, `dream_pack` writes each bundle's files into `<bundle>.pak` beside `assets.pak`, which then
holds only the base. On Android, a bundle's files stay inside the APK.

### The asset_source interface

Every `assets:/` read now goes through the project's `asset_source` module, when it lists one,
after the asset folders the editor adds; without one, the platform reads assets as before. The
interface reads files, gives their versions for hot reload, and mounts and unmounts bundles:

| Module | Reads |
|---|---|
| `bundles` | A bundle's files only while it is mounted. Mounting opens its `<bundle>.pak` beside the program, where packaging made one, and unmounting closes it; elsewhere its files are read through the platform. The base, through the platform |
| `files` | Everything through the platform. Bundles are always mounted, so a game written for bundles runs too |

`bundles` refuses a bundle's files while it isn't mounted, saying which, during development too,
where they are loose files: a game that forgets to mount a bundle fails at once, not only once
packaged. A project that names bundles must list `bundles` first, which the build checks, as a
packaged game's base archive no longer holds the bundles' files.

A game mounts a bundle before it loads anything in it, and unmounts it when done:

```cpp
const std::string bundle(app.asset_database().bundle_of("assets:/scenes/forest.toml"));
if (app.asset_source() != nullptr) {
    app.asset_source()->mount(bundle);
}
```

Unmounting doesn't unload what was loaded: assets stay while the game holds them. The platform
gained `program_folder()`, where a packaged PC game's archives are.

`samples/scene_demo` ships each of its two scenes in a bundle, mounting one as it opens it and
unmounting the other; the pipeline both use stays in the base.

## Consequences

- A broken reference fails the build, with the file and the name, rather than the game.
- A game can learn what a scene needs before loading it, as a loading screen or a preloader would.
- PC games can ship levels, chapters or downloadable content as archives of their own, and a
  patch can replace one archive.
- The database adds a moment to each assets build: it reads every staged file it can scan.
- References made in code, such as `assets:/ui/hud.rml`, aren't in the database, so it can't tell
  which assets nothing uses.
- Still to do: bundles as Play Asset Delivery packs on Android, downloaded on demand; streaming,
  loading a bundle on a thread of its own; importers for more formats; listing the database in the
  editor's asset browser; and the per-platform builds the database describes, made side by side.
