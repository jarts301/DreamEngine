# 0008. Graphics interface and shaders

- Status: Accepted, extended by [0013](0013-directx-and-shader-formats.md),
  [0016](0016-profiling-resources.md), which adds the optional `memory_usage` query, and
  [0017](0017-gpu-frame-time.md), which adds the optional `take_gpu_frame_seconds` query
- Date: 2026-11-09

## Decision

### Interface

`interfaces/rhi` is deliberately small: buffers, shaders, pipelines, push constants and one render
pass that draws to the window. It grows only when a feature has a user and every backend can
implement it. There are two backends: `vulkan` and `null`, which draws nothing and enforces the
interface's rules for automated runs.

The platform layer exposes the operating system's window as plain pointers, and the device builds
its surface from those. This keeps the two backends independent: Vulkan doesn't know about SDL.

### Vulkan baseline

The backend targets Vulkan 1.1, using classic render passes, because Android 10 devices aren't
guaranteed anything newer. Vulkan is loaded at run time through volk, so a machine without a driver
can fall back to another backend. Memory is managed by VMA.

Clip space follows ADR 0005 through a negative viewport height, so nothing above the backend knows
about Vulkan's +Y-down convention.

### Surface loss

Android can destroy the surface before the engine hears about it, so presenting can fail with
out-of-date or surface-lost errors at any moment. Those are normal: the device skips frames until
the main loop detaches and reattaches. Android's loader logs `dequeueBuffer failed` when this
happens, which is harmless. Device resources survive; only the swapchain is rebuilt.

The swapchain asks for no pre-rotation, so Android's compositor rotates the image on rotated
displays. That costs some performance on phones and can be optimized without changing the API.

### Shaders

Shaders are written in Slang and compiled at build time by `dream_add_game`. Each file defines
`vertex_main` and `fragment_main`. Compiled files are ordinary game assets, named with the suffix
that `rhi::Device::shader_file_suffix` reports, so a future backend can ship its own format.

### Lifecycle events

SDL gives Android's lifecycle events only to event watchers, and never queues them. The platform
backend registers a watcher and keeps its own queue, and `wait_event` wakes regularly to check it.

## Consequences

- Buffers started in CPU-visible memory. ADR 0010 moved them to staging uploads.
- Destroying a buffer or pipeline waits for the GPU. A deletion queue can replace that if streaming
  needs it.
- Build servers have no GPU, so CI covers the `null` backend. The real Vulkan path is checked by
  running `dream_cube_vulkan_check` or the cube APK on real hardware.
- Validation layers are an optional vcpkg feature, `vulkan-validation`, because they are slow to
  build and only developers need them.
