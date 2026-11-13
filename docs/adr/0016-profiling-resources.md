# 0016. Resources in profiles: frame timing, memory and counters

- Status: Accepted, extended by [0017](0017-gpu-frame-time.md), which adds the GPU's frame time
- Date: 2026-11-12

## Context

ADR 0015 records where time goes, as zones. Finding why a game stutters or grows also needs the
frame pacing itself, memory as the system, the heap and the GPU see it, and how much work each
system had. These are values over time, which profiles already hold as counters
(`DREAM_PROFILE_VALUE`). What's needed is for the engine to record them, on Windows and Linux,
without games writing code, and at little cost.

[docs/reports/reading-profiles.md](../reports/reading-profiles.md) explains how to read each one.

## Decision

### Where each value comes from

| Values | Recorded | Source |
|---|---|---|
| `Frame time (ms)`, `Frame CPU work (ms)`, `Fixed steps per frame` | Every frame | The main loop |
| `Heap allocations per frame`, `Heap allocated per frame (KB)`, `Heap in use (MB)` | Every frame | The counting operator new, below |
| `Memory resident (MB)`, `Memory private (MB)`, `CPU usage (%)` | `sample_rate` times a second | The operating system, through `core::process_memory` and `core::process_cpu_seconds` |
| `GPU memory (MB)`, `GPU memory budget (MB)` | The same | `rhi::Device::memory_usage` |
| `Tracked <tag> (MB)` | The same, for tags that have been used | `core::memory_stats` |
| `Draw calls`, `Debug lines`, `UI draw commands`, `Physics 3D bodies`, `Physics 3D contact events` and the 2D ones, `Audio voices`, `World transforms` | Each time the system runs | The runtime's systems |

```toml
[profiler]
sample_rate = 10  # Times per second for the sampled values. At most 1000.
```

A frame's values are recorded after its `Frame` zone and before its frame mark, so a profiler that
records a range of frames keeps each frame's values with it. `runtime::ResourceMonitor` does the
recording. While no profiler is installed, it checks for one and reads nothing. The system and the
driver are asked in a `Profiler sampling` zone, so the cost shows in the profile itself.

### Frame timing

`Frame time` runs from the end of one frame to the end of the next, and after a pause from when
updates resumed, so paused time belongs to no frame. `Frame CPU work` is the frame time less the
time spent in the device's `begin_frame` and `end_frame` and in the frame limiter's sleep, where
the GPU, the display or the limiter sets the pace. Comparing the two tells CPU-bound frames from
the rest.

### Process memory and CPU

Core asks the system directly, in `process_stats.cpp`, as the clock and the log already do. The
platform interface is about windows, events and files, and a null platform still runs in a real
process.

| Value | Windows | Linux |
|---|---|---|
| `Memory resident` | Working set | `VmRSS` |
| `Memory private` | Private bytes: committed, in RAM or the page file | `RssAnon` plus `VmSwap` |
| `CPU usage` | `GetProcessTimes`, updated once per scheduler tick of about 15 ms | `CLOCK_PROCESS_CPUTIME_ID` |

### Counting the heap

A game whose configuration lists a profiler for the platform being built gets
`runtime/entry/heap_counting.cpp`, which replaces the global operator new and delete. It counts
every allocation, from any thread and library, into atomics in Core, read by `core::heap_stats`.
Like `game_entry.cpp`, it is compiled into the game, because a replacement in a static library
might not be linked.

- Only games that list a profiler get it, because it costs a little on every allocation: three
  atomic additions, plus asking the allocator for the block's size. Removing the list, or
  `DREAM_PROFILING=OFF`, removes it.
- Blocks are measured by the allocator's own size (`malloc_usable_size`, `_msize`), so a release
  subtracts exactly what its allocation added even when delete isn't told the size.
- Allocations over the default alignment use `posix_memalign` or `_aligned_malloc`, and both
  directions decide by the same rule, because Windows needs `_aligned_free` for those.
- Without exceptions there is no `std::bad_alloc`: a failed allocation calls the new-handler, if
  one is set, and otherwise aborts.

It counts C++ allocations only. `malloc` from C libraries and drivers is in the process's memory
figures but not in the heap's.

### Video memory

`rhi::Device::memory_usage` is an optional capability (ADR 0001, rule 5): it returns nothing by
default, so the null device and other backends need no change.

- **DirectX** asks DXGI (`IDXGIAdapter3::QueryVideoMemoryInfo`) for the process's use of the GPU's
  local memory and the budget Windows grants it.
- **Vulkan** enables `VK_EXT_memory_budget` when the GPU has it and tells VMA, which then reports
  the driver's figures. Without the extension, VMA reports only what it allocated itself, and its
  own estimate of the budget. Only device-local heaps are summed.

### Summaries

The `timeline` backend sums each value up when it writes the trace: samples, minimum, mean, 50th,
95th and 99th percentile by nearest rank, maximum, and the last sample. They go into the trace's
`otherData` under `values`, and into the log, one line per value.

## Consequences

- Every profile of a game shows frame pacing, memory and CPU use without any game code. A profile
  of the hello game running 120 frames at 60 FPS, in the Linux container where this was written,
  summed up to a mean frame time of 16.74 ms with a p99 of 22.6 ms, and no heap allocations in most
  frames.
- Sampling cost about 0.1 ms each time in that run, about 0.1% of the time at 10 samples a second.
- The profiler's own storage shows up as heap allocations: one block of 48 KB for every 1024 events.
- Video memory figures are not comparable between backends, or on Vulkan between GPUs with and
  without the memory budget extension. On integrated GPUs, video memory is system memory.
- `CPU usage` on Windows moves in steps of the scheduler tick, so single samples are rough.
- GPU time was not measured here. ADR 0017 adds it.
- The Vulkan and DirectX parts could not be compiled where this was written, since neither SDK was
  available. They use only documented VMA 3 and DXGI 1.4 calls.
