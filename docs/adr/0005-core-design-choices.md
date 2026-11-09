# 0005. Core design choices

- Status: Accepted
- Date: 2026-11-09

## Decision

### Math conventions

Every module must follow these, or convert at its boundary.

| Topic | Convention |
|---|---|
| Coordinates | Right-handed, +Y up, the camera looks down -Z |
| Matrices | Column-major storage, column vectors, so `a * b` applies `b` first |
| Quaternions | `a * b` applies `b` first; positive angles follow the right-hand rule |
| Clip space | +Y up, depth 0 at the near plane and 1 at the far plane |
| Angles | Radians everywhere |

Vulkan's clip space has +Y down. The Vulkan backend compensates with a negative viewport height, so
nothing above the graphics layer needs to know.

The math library is written in-house as plain float types with no dependencies. SIMD can be added
later behind the same functions.

### Configuration format

Project configuration is TOML, which allows comments. Parsing uses toml++ in header-only mode inside
`config.cpp`, and the result is copied into plain standard-library types, so no toml++ type reaches
a public header. Core reads text only; loading files is left to the platform layer, because Android
assets are not ordinary files.

### Job system

The first version is a single mutex-protected queue. Waiting threads run queued jobs instead of
blocking, so jobs can safely wait for other jobs. Jobs are stored inline, without allocating, which
limits captures to small trivially copyable data.

## Consequences

- A single queue will become a bottleneck with many short jobs. A work-stealing scheduler can replace
  it without changing the public API.
- Core has one private third-party dependency, toml++.
