# 0017. GPU frame time

- Status: Accepted
- Date: 2026-11-12

## Context

ADR 0016 records how long the CPU spends on each frame, but not the GPU. Without the GPU's time, a
slow frame whose CPU work is small could be the GPU, the display or the operating system, and the
profile can't say which. Both graphics backends can timestamp the GPU's work. Games are profiled on
PC, mostly on Windows, where both backends run.

Three levels were considered: the whole frame's GPU time, a breakdown by part of the frame, and GPU
work drawn on the timeline beside the CPU's zones. The last needs the GPU's clock matched to the
CPU's, which Vulkan offers only through an optional extension (`VK_EXT_calibrated_timestamps`) and
Direct3D 11 not at all. The whole frame is the part every backend can do exactly, and the one that
answers whether a frame is GPU-bound.

## Decision

### The interface

`rhi::Device::take_gpu_frame_seconds` is an optional capability (ADR 0001, rule 5), returning
nothing by default. It hands out each measured frame's GPU time once, oldest first. Backends keep
the last 16 untaken times and drop older ones, so memory stays bounded whoever calls it.

The GPU runs behind the CPU, so a frame's time is known only a few frames after it was recorded:
two with Vulkan's two frames in flight, up to about four with Direct3D 11's queue. The backends
read their timestamps only once the GPU has finished with them, so measuring never makes the CPU
wait.

### DirectX

Each frame takes a set of queries from a ring of four: a disjoint query around the frame, and a
timestamp at the start of its commands and one at their end, before presenting. Sets are read back
with `D3D11_ASYNC_GETDATA_DONOTFLUSH`, oldest first, until one isn't ready. A frame whose interval
is reported disjoint, as when the GPU changes its clock, has no time. If every set is still waiting
for the GPU, the frame goes untimed rather than waiting.

### Vulkan

Each frame slot owns two timestamps in one query pool, reset and written at the start of the
frame's command buffer and written again at its end. They are read after `begin_frame` has waited
for the slot's fence, so they are always ready. Ticks are masked to the queue's
`timestampValidBits` and scaled by the device's `timestampPeriod`. A queue without timestamps turns
timing off.

### Recording

`runtime::ResourceMonitor` takes every available time after each frame and records it as
`GPU frame time (ms)`. It takes them even while no profiler is installed, so a profiler installed
later never gets stale times.

## Consequences

- Comparing `GPU frame time` with `Frame time` and `Frame CPU work` tells GPU-bound frames from
  CPU-bound ones. [docs/reports/reading-profiles.md](../reports/reading-profiles.md) shows how.
- The value's graph is two to four frames behind the frame it measures. Summaries are unaffected.
- It measures the GPU from the start of the frame's commands to their end. Presenting and the
  desktop's compositor come after, and aren't counted. On Vulkan, if the GPU reaches the frame's
  drawing before the window's image is free, that wait is counted.
- A GPU that lowers its clock when lightly loaded reports longer times for light frames than the
  same work would take at full speed.
- Each frame costs two timestamps and, on DirectX, three queries read back later.
- `dream_rhi_directx_tests` checks the DirectX timing on WARP, Windows' software rasterizer. The
  Vulkan timing has no automated test, since build servers have no GPU. Running
  `dream_cube_vulkan_check` with validation layers on checks it.
- A breakdown by part of the frame, and GPU work on the timeline, remain possible later. The first
  needs timestamps in the command list, and the second a way to match the GPU's clock.
