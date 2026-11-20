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
| [0020](0020-first-game.md) | Toppler, the first game, and what it changed in the engine |
| [0021](0021-screenshot-tests.md) | Screenshot tests: frame capture in the graphics backends, compared with reference images |
| [0022](0022-licenses-and-frozen-libraries.md) | Wider third-party licenses, frozen in-house libraries, and Windows and Android as the targets |
| [0023](0023-direct3d-12.md) | A Direct3D 12 backend beside Direct3D 11, loading the same shaders |
| [0024](0024-texture-cooking.md) | Textures cooked to KTX2 and transcoded to BC7 or ASTC, and PC games packaged with an asset archive |
| [0025](0025-hot-reload.md) | Hot reloading shaders and assets while a game runs |
| [0026](0026-opengl.md) | OpenGL and OpenGL ES backends, with shaders compiled through SPIRV-Cross |
| [0027](0027-render-targets.md) | Render targets, three texture slots with a shadow comparison, and depth-only pipelines |
| [0028](0028-hdr-and-tone-mapping.md) | The renderer's HDR scene, its format per device, and Khronos PBR Neutral tone mapping |
| [0029](0029-directional-shadows.md) | Shadows from the directional light, on by default per device |
| [0030](0030-renderer-module.md) | The renderer is a module, chosen in `dream.toml` |
| [0031](0031-frustum-culling.md) | Parts out of the camera's view, or out of a shadow cascade, are skipped |
| [0032](0032-instancing-and-lods.md) | Repeated parts are drawn as instances, and distant ones with simpler versions |
| [0033](0033-pbr-materials-and-normal-maps.md) | Materials are glTF's metallic-roughness PBR, with normal maps and tangents from a module |
| [0034](0034-transparency.md) | Blended, cut-out and double-sided materials, drawn in passes after opaque ones |
| [0035](0035-gamepads.md) | Gamepads through an `input_device` module, bound to the same actions as keys, with analog values |
| [0036](0036-skeletal-animation.md) | Skeletal animation through an `animation` module (ozz-animation), with GPU skinning |
| [0037](0037-point-and-spot-lights.md) | Point and spot lights, sorted into clusters on the CPU, in `forward` and `forward_plus` |
| [0038](0038-particles.md) | Particles: a `particles` interface simulated on the CPU, drawn as camera-facing squares |
| [0039](0039-sprites-and-tile-maps.md) | 2D sprites, sprite-sheet animation and tile maps, drawn by the renderer modules over the scene |
| [0040](0040-environment-lighting-and-bloom.md) | Environment lighting from prefiltered HDR images, an eighth texture slot, half-float textures, and bloom |

## Template

```markdown
# NNNN. Title

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context
## Decision
## Consequences
```
