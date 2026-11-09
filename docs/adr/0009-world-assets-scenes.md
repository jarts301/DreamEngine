# 0009. World, events, assets and scenes

- Status: Accepted
- Date: 2026-11-09

## Decision

### Entities

The entity system is EnTT, used directly through `runtime/ecs.h`. As ADR 0001 says, it isn't
swappable: games and the engine share it as vocabulary, so wrapping it would only add a layer to
maintain. `runtime::World` adds what EnTT doesn't have: names and a parent hierarchy, with loops
rejected when a link is made. Systems are plain game code that loops over a view.

### Events

`runtime::EventBus` is written in-house rather than using EnTT's dispatcher, for two properties:
listeners can be lambdas, and a `Subscription` disconnects automatically. A listener can't outlive
what it captured. Events can be delivered at once with `publish`, or once per frame with `enqueue`.
Every platform event also goes through the bus.

### Assets

`AssetManager` loads through a loader registered per type, and returns `std::shared_ptr<const T>`.
It keeps only weak references, so an asset is freed when nothing uses it and no manual unloading is
needed. Loaders can load other assets, and a chain that depends on itself is an error.

Loading is synchronous, because the platform's file functions belong to the main thread. A game
object is destroyed before the graphics device, so GPU assets it holds are released safely.

### Scenes

A scene is a TOML file that lists entities and their components. Games register a reader for each
of their component types. The whole file loads into a new `World`, so a mistake leaves the current
scene untouched, and a new scene is ready before the old one is dropped. Assets the two share aren't
reloaded.

Anything a scene file doesn't recognize is an error, so typos are reported rather than ignored.

## Consequences

- Large scenes block the frame while loading. Background loading needs thread-safe file access in
  the platform interface first.
- Scenes can be loaded but not saved. Saving needs a writer for each component.
- Meshes, textures and materials don't exist as engine assets yet. Those arrive with the renderer,
  and `samples/scene_demo` shows the pattern with its own GPU assets.
