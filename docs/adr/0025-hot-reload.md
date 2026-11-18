# 0025. Hot reloading shaders and assets

- Status: Accepted
- Date: 2026-11-17

## Context

Changing a shader, texture or model meant closing the game, building and starting it again, and
getting back to the moment being worked on. Games read their assets through the asset manager, and
the build already turns sources into what games read: shaders into `.spv` and `.dxbc`, textures into
`.ktx2`, staged next to the program by the `<game>_assets` target. That target can run while the
game does, since it doesn't touch the program, which Windows keeps locked.

## Decision

While hot reloading is on, the App checks the files behind every asset still in use a few times a
second, between frames, and loads again those whose files changed:

1. Edit a shader, texture or model, and save.
2. `cmake --build <folder> --target <game>_assets`, or build that target in the IDE.
3. The running game picks it up within half a second, and logs "Reloaded '...'".

```toml
[hot_reload]
enabled = true      # Debug builds on PC by default; off in release builds and on Android.
interval_ms = 500
```

Screenshot tests turn it off, since they compare a fixed moment.

### How it works

- **Which files.** The virtual file system tells the asset manager of every read while an asset
  loads, including reads that fail, so a model that looked for a cooked texture and fell back to the
  PNG reloads once the cooked one appears. Each file belongs to the innermost asset loading, so a
  model reloads for its own files, and a dependency reloads for its.
- **When they change.** `Platform::file_version` gives a file's modification time. It is an
  optional capability: the SDL platform has it on PC, and nothing has it for files inside an APK or
  an `assets.pak`, so packaged and Android games never reload.
- **Every reference sees the new version.** `AssetRef<T>` now points to a slot that all references
  to one asset share, and a reload replaces what the slot holds. The old version is freed when
  nothing uses it, which for GPU resources is between frames, as destroying requires.
  `AssetRef::version` counts the reloads.
- **Failures keep the last version.** A loader that fails, such as on a shader that doesn't
  compile for the device, leaves the asset as it was and logs why. It is tried again when its files
  change again.
- **The renderer's shaders** are loaded as one asset, `RendererPipelines`: its six compiled shaders
  and the pipelines made from them. A change to any of them rebuilds them all.
- **Games** hear of a reload through an `AssetsReloaded` event with the paths, for anything they
  built from an asset themselves.

## Consequences

- Checked by hand on the render demo: changing `forward.slang`, then the crate's texture, and
  building the assets target, changed the running game within a second, and putting them back
  restored it.
- Assets built from other assets, such as the scene demo's pipelines from its shader assets, keep
  using what they were built from until they rebuild, which they can do on `AssetsReloaded` or by
  comparing `version()`.
- Files read outside the asset manager don't reload: the cube sample's own shaders, scenes, and the
  documents and style sheets of the game interface.
- Reloading follows the build: a source file put back with an older modification time, as copying
  a backup over it can do, isn't rebuilt, so nothing reloads until it is saved again.
