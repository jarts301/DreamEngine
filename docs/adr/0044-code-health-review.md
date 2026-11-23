# 0044. Code health review: per-world state, shared readers, and the texture decoder module

- Status: Accepted
- Date: 2026-11-23

## Context

A review of the engine for bugs, duplicated code and features that should be modules, with
clang-tidy's `bugprone`, `performance` and static analyzer checks over the engine's 90 source files
for Android, and a read of the code it can't judge.

It found these bugs:

- **`EventBus::flush` couldn't be re-entered.** The allocation work of ADR 0041 swapped the queue
  with a member list kept for delivering. A listener that flushed again, to deliver what it had
  just queued, swapped that member list out from under the loop that was walking it.
- **Animation and particles kept state by the world's address.** Each keeps a player or an
  emitter per world and entity. A world destroyed and another made in its place, as loading a level
  often does, very often has the same address, and its entities the same numbers, so the new world
  inherited the old one's animation time and particles. The state of a destroyed world was also
  never released.
- **The model cook tool always simplified with meshoptimizer.** A platform that lists no mesh
  simplifier, or `null` first, makes no levels of detail as models load, but its build still cooked
  them, so the same game drew differently cooked and uncooked.
- **The sprite pass read the 2D camera without checking for one.** The runtime never sends sprites
  without it, but another caller of the renderer interface could, and the renderer would read an
  empty value.

The rest of clang-tidy's findings were false positives: values checked earlier by a branch the
check can't follow, the deliberate faults of `simulate_crash`, and third-party headers.

It found this duplication:

| What | Where |
|---|---|
| `scene_error` | Six copies, three of them giving `InvalidArgument`, three `ParseError` |
| Reading settings in turn, keeping the first error | Four classes or lambdas: sprites, particles, 2D and 3D physics |
| Recognizing a KTX2 file | `texture.cpp` and `model_parse.cpp` |

And one feature that should be a module: **reading cooked textures**. ADR 0043 made texture
cooking a choice, with `none` among the cookers, but the runtime still linked libktx, with its
Basis Universal transcoder, into every game, including those that cook nothing.

## Decision

### Per-world state

`World` has an `id()`, a number from a counter no other world in the program has had, and a
`lifetime()`, a weak pointer that expires when the world is destroyed. Animation and particles key
their state by `(id, entity)`, and release the state of worlds whose lifetime has expired, through
a small `WorldLifetimes` helper beside `World`. A world that is only paused, not updated while
another is, keeps its state.

### EventBus

A flush moves the queue to a list of its own, delivers from it, and gives the emptied list back
for the next flush to reuse. Events queued during delivery wait for the next flush, and a listener
may flush again without disturbing the one under way. Still no allocations once the lists have
grown.

### Shared scene readers

`scene_loader.h` gives `scene_error`, which is `ParseError`, and `SettingReader`, which reads a
component's settings in turn, keeping the first error: numbers, flags, vectors, text, whole numbers,
integers and angles. Sprites, particles and both physics readers use it; sprites add their own
color, filter and texture settings on top. `is_ktx2` is defined once, in `model_parse.cpp`, which
the model cook tool also builds.

### The texture decoder module

A new interface, `texture_decoder`, reads the textures the build cooked:

| Module | Reads |
|---|---|
| `ktx` | KTX2, plain RGBA8 or Basis Universal, transcoded to BC7, then ASTC 4x4, then RGBA8 |
| `null` | Nothing: every cooked texture fails to load, with a message naming the fix |

The runtime no longer links libktx. The Renderer creates the listed decoder; without one, textures
load from their PNG files, and a cooked texture in a model fails with a message.

A texture cooker file names the decoder that reads what it makes, `DREAM_TEXTURE_COOKER_DECODER`,
`ktx` for `ktx2_uastc` and `ktx2_etc1s`. When the build cooks any texture for a game, it checks
that the game lists that decoder first for the platform, and stops with the line to add if not:

```toml
[modules.android]
texture_decoder = ["ktx", "null"]
```

### Smaller changes

- The model cook tool makes levels of detail with the platform's listed `mesh_simplifier`, and
  writes an empty file, which the runtime ignores, when none is listed. Its vcpkg port's version
  went up, so Android builds rebuild it.
- Sprite gathering keeps its sorting lists in a `SpriteScratch` the Renderer owns, not in
  `thread_local` variables, which outlived every Renderer and grew in every thread that drew.
- The forward renderer's square vertex buffers, which particles and sprites share, are named for
  quads, not particles.
- `AssetManager::store` takes the asset by reference, where a `std::move` into a const reference
  did nothing.

## Consequences

- Levels that are loaded into new worlds start their animations and particles fresh, and the state
  of destroyed worlds is released.
- A game that cooks no textures links no KTX library. Every project that cooks textures must list
  `texture_decoder = ["ktx", ...]`; the build says so, with the line to add, rather than failing
  at run time. The samples that cook textures list it.
- Scene errors are all `ParseError`, where sprites and particles gave `InvalidArgument`.
- Still to do, found but left alone: `forward_renderer.cpp` is 2,400 lines and would read better
  split by pass; the runtime's and the forward renderer's tests each have a fake graphics device;
  and each settings table has its own small `setting_error`, as its prefix differs.
