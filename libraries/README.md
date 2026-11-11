# Libraries

In-house libraries that backends build on, for jobs that no MIT or Apache 2.0 library does. Each is
plain C++: it uses only the standard library and third-party libraries, never the engine, so it
could serve any program. Backends in `modules/` adapt them to the engine's interfaces. See
[ADR 0014](../docs/adr/0014-more-backends-and-libraries.md).

| Library | What it does | Used by |
|---|---|---|
| [impulse](impulse) | 3D rigid-body physics: boxes, spheres and capsules, joints, queries, sleeping | `physics3d/impulse` |
| [markup](markup) | Game interfaces from RML documents and RCSS style sheets, as RmlUi reads them | `ui/markup` |
| [mixer](mixer) | Mixing voices of decoded or streamed audio into stereo | `audio/portaudio` |

A library's CMakeLists.txt calls `dream_add_library`, which builds it with the engine's settings as
`dream::library_<name>`. Public headers go in `include/<name>/`, and tests in `tests/`. Libraries are
always built, so their tests run even when no project uses them.
