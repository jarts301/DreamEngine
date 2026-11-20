# Libraries

In-house libraries that backends build on, written when the engine allowed only MIT or Apache 2.0
code. Each is plain C++: it uses only the standard library and third-party libraries, never the
engine, so it could serve any program. Backends in `modules/` adapt them to the engine's
interfaces. See [ADR 0014](../docs/adr/0014-more-backends-and-libraries.md).

**These libraries are frozen** ([ADR 0022](../docs/adr/0022-licenses-and-frozen-libraries.md)):
they get bug fixes and the changes needed to keep building and passing, but no new features. New
features go to the first backend of each interface: Jolt, RmlUi and miniaudio.

On the `offline-web` branch (ADR 0039), which has no Jolt, RmlUi or audio device, impulse and
markup are the backends games use, and `webserve` is a library of its own, new to that branch.

| Library | What it does | Used by |
|---|---|---|
| [impulse](impulse) | 3D rigid-body physics: boxes, spheres and capsules, joints, queries, sleeping | `physics3d/impulse` |
| [markup](markup) | Game interfaces from RML documents and RCSS style sheets, as RmlUi reads them | `ui/markup` |
| [mixer](mixer) | Mixing voices of decoded or streamed audio into stereo | `audio/portaudio` (main branch only) |
| [timeline](timeline) | Recording timed events from many threads, saved as a Chrome trace | `profiler/timeline` |
| [webserve](webserve) | A small HTTP server on POSIX sockets, for pages that show programs in a browser | `platform/web`, `tools/hub` |

A library's CMakeLists.txt calls `dream_add_library`, which builds it with the engine's settings as
`dream::library_<name>`. Public headers go in `include/<name>/`, and tests in `tests/`. Libraries are
always built, so their tests run even when no project uses them.
