# 0042. Levels of detail made at build time

- Status: Accepted
- Date: 2026-11-20

## Context

Models get simpler versions for drawing far away (ADR 0032), made by the `mesh_simplifier` module
as each model loads. Measured on the A52s, that costs about 1.5 ms per 1,000 triangles: under 3 ms
for all of the samples' small models, but about 8 ms for a model of 10,000 triangles, 50 to 80 ms
for 50,000, and 280 to 345 ms for 200,000, every time a model loads. A level of detailed models
would spend seconds simplifying meshes, on the device least able to.

The build cooks textures with a tool on the build machine (ADR 0024), which vcpkg installs for
it. Packing a PC game's assets uses a tool the engine builds itself, which a cross-compiled build,
such as Android's, can't do: it builds only for the target.

## Decision

### The model cook tool

`tools/model_cook` builds `dream_model_cook`. For a `.glb`, it does what the runtime would do as the
model loads, with the same code:

1. reads the model with `runtime::parse_glb`, from `runtime/src/model_parse.cpp`, which now holds
   everything about reading models that doesn't touch the GPU, split from `model_loader.cpp`;
2. makes its tangents with the tangent generator the game lists for the platform, if any, as they
   can add vertices;
3. makes its simpler versions with the platform's listed `mesh_simplifier` (since ADR 0044;
   none listed, none made) and the game's `[lods]` settings;
4. writes them to a `.lods` file beside the model's place in the game's assets: `models/ball.glb`
   gets `models/ball.lods`.

It compiles the engine's core, the model reading and the two modules from their sources, so it
reads models exactly as the runtime does, in either of two builds:

| Build | Where the tool comes from |
|---|---|
| PC | The engine's build, like `pack`: always current |
| Android, and other cross-compiled builds | `cmake/ports/dream-model-cook`, an overlay port that builds it from this repository for the build machine, as `ktx` is built |

vcpkg can't see the port's sources change, so its version must be raised when `model_parse.cpp` or
the tool change. A stale tool can't do harm, as the runtime checks what it made.

### In the build

`dream_add_game` cooks every `.glb` in the game's asset folders into its generated assets, beside
cooked textures, whenever the model, the game's `dream.toml` or the tool changes. `[lods] cook =
false` makes empty files, which the runtime ignores, and the build option `DREAM_COOK_MODELS=OFF`
skips cooking. Without the tool, nothing is cooked.

### At run time, with a way back

The model loader reads a model's `.lods` file, when there is one, in place of making its versions.
The file records what it was made from: the model's size and a 64-bit FNV-1a hash of its bytes, the
`[lods]` settings, the vertex count after tangents, every part's indices, and the format version.
If any differs, because the model was edited and hot-reloaded, a setting changed, or the engine
reads models differently since, the file is refused with a warning naming why, and the versions
are made as the model loads, as before. With no file, the same.

So making versions as models load remains, unchanged, as the fallback, and `cook = false` goes back
to it entirely.

## Consequences

- Detailed models load several times faster. Measured on the A52s, loading each model in full,
  three runs each:

  | Model | Triangles | Making versions as it loads | Reading cooked ones | Whole load, before | Whole load, cooked |
  |---|---|---|---|---|---|
  | Test sphere | 10,000 | 7.8 ms | 1.0 ms | 9.3–9.7 ms | 2.6 ms |
  | Test sphere | 50,000 | 52–81 ms | 4.2–4.3 ms | 58–91 ms | 8.4–9.2 ms |
  | Test sphere | 200,000 | 282–345 ms | 19–20 ms | 318–392 ms | 55–67 ms |
  | The samples' ball | 768 | 0.6 ms | 0.1 ms | 1–5 ms | 0.6–4.5 ms |

  Reading cooked versions includes reading the `.lods` file and hashing the model: about 20 ms for
  the 200,000-triangle model's 5.6 MB and its 2.1 MB of versions.
- A game that cooks every model can drop `mesh_simplifier` from its platform's module list, as the
  cooked versions need no simplifier at run time.
- Builds take longer the first time, and each time a model changes: about as long as the
  simplifying took at load on the build machine, which is several times faster than a phone.
- A model's assets grow by its versions' indices: with the default settings, nearly as much
  again as its own index data (half, a quarter and an eighth of it).
- Hand-made levels of detail, such as glTF's `MSFT_lod`, are still to come. They would take
  priority over cooked ones.
