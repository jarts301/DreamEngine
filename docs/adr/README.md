# Architecture decision records

Each file records one decision that shapes the engine: what was decided, why, and what it costs.
Changing a decision means adding a new record that supersedes the old one, not rewriting history.

| ADR | Decision |
|---|---|
| [0001](0001-layered-architecture.md) | Layers, dependency direction and module rules |
| [0002](0002-cpp-subset.md) | The C++20 subset the engine uses |
| [0003](0003-naming-and-layout.md) | Naming and repository layout |
| [0004](0004-initial-targets.md) | First targets: 3D, SDL3 backend, Android API 29 |
| [0005](0005-core-design-choices.md) | Math conventions, configuration format, job system |
| [0006](0006-module-system.md) | Module selection, registration and the layer check |
| [0007](0007-platform-and-game-loop.md) | Platform layer, lifecycle, game loop and APK packaging |
| [0008](0008-rhi-and-shaders.md) | Graphics interface, Vulkan baseline and shader pipeline |
| [0009](0009-world-assets-scenes.md) | Entities, events, assets and scene files |
| [0010](0010-renderer-v1.md) | Forward renderer, models and textures |
| [0011](0011-physics.md) | 3D and 2D physics, fixed timestep and debug drawing |
| [0012](0012-audio-and-ui.md) | Audio, game and debug interfaces drawn by the renderer |
| [0013](0013-directx-and-shader-formats.md) | DirectX backend, shader formats per backend and shader bindings |
| [0014](0014-more-backends-and-libraries.md) | Second physics, audio and interface backends, and in-house libraries |
| [0015](0015-profiling.md) | Profiling zones in Core, a swappable profiler and Chrome trace files |
| [0016](0016-profiling-resources.md) | Frame timing, memory, CPU and engine counters in profiles |
| [0017](0017-gpu-frame-time.md) | GPU frame time, measured with timestamps on DirectX and Vulkan |
| [0018](0018-memory-budgets.md) | Memory budgets, pressure levels and an emergency reserve against running out of memory |
| [0019](0019-crash-reports.md) | Crash reports and minidumps, written without allocating, and simulated crashes |

## Template

```markdown
# NNNN. Title

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context
## Decision
## Consequences
```
