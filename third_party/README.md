# Third-party libraries

Everything the engine needs from outside is in this folder, so a build works with no network and
no package manager (ADR 0039). Each library keeps its license file, whose notice must stay with it,
including in anything shipped that contains its code.

All of them are under permissive licenses, as ADR 0022 requires: they may be copied, changed and
shipped, in source or compiled, in free or commercial programs, as long as the notice stays. None
is copyleft.

## Header-only

| Folder | Library | Version | License | Used by |
|---|---|---|---|---|
| `doctest` | [doctest](https://github.com/doctest/doctest) | 2.4.12 | MIT | every test program |
| `entt` | [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | runtime (the world's entities) |
| `tomlplusplus` | [toml++](https://github.com/marzer/tomlplusplus) | 3.4.0 | MIT | core (configuration files) |
| `cgltf` | [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | runtime (glTF models) |
| `stb` | [stb](https://github.com/nothings/stb) `stb_image.h`, `stb_image_write.h`, `stb_truetype.h` | 2026-08-02 | MIT or public domain | runtime (PNG), markup (fonts) |

## Compiled from source

Built by `third_party/CMakeLists.txt` with the engine. Only the library sources are kept: no
samples, tests, tools or build scripts of their own.

| Folder | Library | Version | License | Used by |
|---|---|---|---|---|
| `box2d` | [Box2D](https://github.com/erincatto/box2d) | 3.1.1 | MIT | `physics2d/box2d` |
| `ozz` | [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) base, runtime and offline | 0.17.0 | MIT | `animation/ozz` |
| `meshoptimizer` | [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | MIT | `mesh_simplifier/meshoptimizer` |
| `mikktspace` | [MikkTSpace](https://github.com/mmikk/MikkTSpace) | 3e895b4 | zlib-style (in `LICENSE.txt` and the header) | `tangent_generator/mikktspace` |
| `imgui` | [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | MIT | `debug_ui/imgui` |

The fonts the samples use are Lato, under the SIL Open Font License, in
`samples/assets/fonts/LICENSE-Lato.txt`.

## Updating one

Replace its files and license with the new release's and note the version here. Nothing else in
the build names a version.

Libraries that need a GPU, a sound card, a display or a native windowing toolkit (Vulkan, Direct3D,
OpenGL, SDL3, miniaudio, PortAudio, RmlUi) are not part of this build, and neither are Jolt,
Chipmunk2D and KTX. Every interface keeps a backend that works here; see README.md.
