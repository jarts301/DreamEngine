# Third-party libraries

Everything the engine needs from outside is in this folder, so a build works with no network and
no package manager (ADR 0039). Only header-only libraries are kept, each with its license.

| Folder | Library | Version | License | Used by |
|---|---|---|---|---|
| `doctest` | [doctest](https://github.com/doctest/doctest) | 2.4.12 | MIT | every test program |
| `entt` | [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | runtime (the world's entities) |
| `tomlplusplus` | [toml++](https://github.com/marzer/tomlplusplus) | 3.4.0 | MIT | core (configuration files) |
| `cgltf` | [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | runtime (glTF models) |
| `stb` | [stb](https://github.com/nothings/stb) `stb_image.h`, `stb_image_write.h`, `stb_truetype.h` | 2026-08-02 | MIT or public domain | runtime (PNG), markup (fonts), web platform (PNG) |

To update one, replace its headers and license file with the new release's and note the version
here. Nothing else in the build names a version.

Libraries that need a GPU, a sound card, a display or a native windowing toolkit (Vulkan, Direct3D,
OpenGL, SDL3, miniaudio, PortAudio, RmlUi, ImGui) are not part of this build, and neither are the
compiled libraries the main branch takes from vcpkg (Jolt, Box2D, Chipmunk2D, ozz-animation,
meshoptimizer, MikkTSpace, KTX). Every interface keeps a backend that works here; see README.md.
