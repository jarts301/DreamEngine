# Report: the profiler's resource values, and how to read them

## Summary

Profiles now record resources as well as time, with no code in the game:

| Group | Values | How often |
|---|---|---|
| Frame timing | `Frame time (ms)`, `Frame CPU work (ms)`, `Fixed steps per frame` | Every frame |
| GPU time | `GPU frame time (ms)` | Every frame, arriving 2 to 4 frames late |
| Heap | `Heap allocations per frame`, `Heap allocated per frame (KB)`, `Heap in use (MB)` | Every frame |
| Process | `Memory resident (MB)`, `Memory private (MB)`, `CPU usage (%)` | 10 times a second |
| GPU | `GPU memory (MB)`, `GPU memory budget (MB)` | 10 times a second |
| Engine budgets | `Tracked <tag> (MB)` | 10 times a second, once used |
| Engine systems | `Draw calls`, `Debug lines`, `UI draw commands`, `Physics 3D bodies`, `Physics 3D contact events`, the 2D ones, `Audio voices`, `World transforms` | Each time the system runs |

Each value is a number over time. The most important rules for reading them:

1. **Compare `Frame time` with `Frame CPU work` and `GPU frame time`.** Whichever of the two is
   close to the frame time sets the frame rate: the game's CPU work, or the GPU. If neither is, the
   display or the frame limiter does, and the game has room to spare.
2. **Judge smoothness by p99 and max, not by the mean.** A mean of 16.7 ms can hide stutters.
3. **A game in steady play should make close to zero heap allocations per frame.** Allocations
   every frame are the usual cause of slow, hard-to-see frame time growth and stutter.
4. **Memory that climbs and never falls, over minutes, is a leak candidate.** Memory that climbs and
   then levels off is a cache or a pool filling up.
5. **The GPU numbers depend on the graphics backend**, so compare runs on the same backend only.

The design is recorded in [ADR 0016](../adr/0016-profiling-resources.md) and, for the GPU's time,
[ADR 0017](../adr/0017-gpu-frame-time.md).

## 1. Getting the values

List a profiler in the game's `dream.toml`, as in [ADR 0015](../adr/0015-profiling.md):

```toml
[modules.windows]
profiler = ["timeline"]

[profiler]
sample_rate = 10     # Optional. How often memory, CPU and the GPU are sampled, per second.
```

Listing a profiler also builds the game with heap counting (section 4.2). Without the list, none of
this costs anything.

There are three places to read the values:

- **In Perfetto.** Open the trace at https://ui.perfetto.dev. Each value is a counter track under
  the game's process, drawn as a graph. Chrome's format stores each sample in a field called
  `value`, and viewers may add that word to the track's name, as in `Frame time (ms) value`.
- **In the log.** When the game ends, the `timeline` backend writes one line per value:

  ```
  Profile values (samples: min / mean / p95 / p99 / max):
    Frame time (ms): 120: 0.006894 / 16.74 / 16.96 / 22.62 / 41.25
  ```

- **In the trace file.** The same summary, with the 50th percentile and the last sample too, is in
  the JSON under `otherData` → `values`, for scripts and CI checks.

### Reading a summary line

`Frame time (ms): 120: 0.006894 / 16.74 / 16.96 / 22.62 / 41.25` means 120 samples, with minimum
0.007 ms, mean 16.74 ms, 95% of frames at 16.96 ms or less, 99% at 22.62 ms or less, and the worst at
41.25 ms. Percentiles use the nearest rank: p95 is an actual sample, the smallest that at least
95% of samples don't exceed.

- **Mean** is the average. It is good for totals, such as average FPS (1000 / mean frame time), and
  poor for smoothness, because a few long frames barely move it.
- **p95 and p99** are what players feel. At 60 FPS, p99 covers all but about one frame in every
  1.7 seconds. If p99 is well above the mean, the game stutters even when the average looks fine.
- **Max** is the single worst sample. Look it up on the timeline to see what happened then.
- **Samples** differs between values on purpose. Frame values have one per frame, sampled values
  about `sample_rate` a second, and `Heap ... per frame` values one fewer than the frames, since
  the first frame has nothing to compare with. `GPU frame time` has a few fewer than the frames,
  because the last frames are still on the GPU when the game ends (section 2).

The summary covers exactly what was recorded, so with `start_frame` and `frames` under
`[profiler.timeline]` it covers only those frames, leaving loading out.

## 2. Frame timing

### Frame time (ms)

How long each frame took, from the end of the previous frame to the end of this one. This is the
time the player sees between two frames, so 16.7 ms is 60 FPS and 33.3 ms is 30 FPS.

- **It includes waiting.** With vertical sync or `[app] max_fps`, a fast game's frame time sits at
  the display's or the limit's period. That is the goal, not a problem.
- **Time spent paused belongs to no frame.** After a pause, the first frame counts from when
  updates resumed, so pausing doesn't appear as a huge frame.
- **A very short frame right after a very long one** is usually the frame limiter re-aligning. It
  doesn't sleep to make up for a late frame, so the frame after a hitch is short. The worked example
  in section 9 shows one.

### Frame CPU work (ms)

The part of the frame time the main thread spent working: the frame time less the time in
`Begin GPU frame`, `Submit and present` and `Frame limit`. It covers events, fixed updates,
updates, recording the frame's drawing, and the engine's own profiling work.

How to tell what limits the frame rate, with `GPU frame time` (below):

| Frame time | Frame CPU work | GPU frame time | What sets the frame rate |
|---|---|---|---|
| At the display's or `max_fps` period, steady | Well below it | Well below it | Nothing is struggling: the game is paced. The headroom is the period less the larger of the two |
| Above the period | Close to the frame time | Below it | The CPU. Look at `Update`, `Fixed update`, `Render` and their children |
| Above the period | Well below it | Close to the frame time | The GPU. Look at what is drawn: `Draw calls`, geometry, screen size, shaders |
| Above the period | Well below it | Well below it | Neither is busy, so something is waited on: a display refresh just missed, the driver, or the operating system. Look at `Begin GPU frame` and `Submit and present` |
| A spike | No spike | A spike, 2 to 4 frames later on its graph | The GPU, on that frame |
| A spike | No spike | No spike | The operating system or the driver, not the game |
| A spike | The same spike | | The game's own work. Find the frame on the timeline and open its `Frame` zone |

The CPU and the GPU work at the same time, the GPU on one frame while the CPU prepares the next,
so a frame takes about as long as the slower of the two, not their sum.

Two things to know:

- `Submit and present` counts as waiting, because with vertical sync presenting waits for the
  display, especially on DirectX. It also holds some of the driver's CPU work, so a driver-heavy
  frame reads as slightly more waiting than it really is.
- Work on other threads, such as physics workers and audio, isn't in this value. `CPU usage`
  covers every thread (section 5).

### GPU frame time (ms)

How long the GPU worked on each frame, from the start of the frame's commands to their end,
measured with the GPU's own timestamps. It is the GPU's counterpart to `Frame CPU work`: when it is
close to `Frame time`, the game is GPU-bound, and drawing less, or more cheaply, makes it faster.

- **It arrives late.** The GPU runs behind the CPU, so a frame's time is known only 2 frames later
  on Vulkan and up to about 4 on DirectX. On the timeline its graph is shifted right by that much:
  a GPU spike belongs to a frame a little to its left. The summary is unaffected.
- **Presenting isn't in it.** Showing the image and the desktop's compositor come after the frame's
  commands.
- **On Vulkan it can include a wait for the window's image.** If the GPU starts drawing before the
  display has released the image it draws to, the wait is counted. A GPU frame time that sits at the
  display's period while little is drawn is that wait, not work. DirectX doesn't show this.
- **Light frames can read long.** A GPU that isn't busy lowers its clock to save power, so the same
  work takes longer when there is little of it. Compare optimizations in scenes that load the GPU.
- **A few frames have no time.** The last frames are still on the GPU when the game ends. DirectX
  also drops a frame when its interval is reported unreliable, as when the GPU changes its clock,
  and when the GPU is more than about 4 frames behind.
- **It is absent** with the `null` graphics backend, and on a GPU whose queue has no timestamps,
  which the log says at startup.

### Fixed steps per frame

How many times `Game::on_fixed_update` ran in the frame (ADR 0011). At a frame rate equal to
`[app] fixed_rate` it should be 1, with an occasional 0 or 2 from timing jitter.

- **Often 2 or more** means frames are slower than the fixed rate, so each frame runs more
  simulation to catch up. That adds work to slow frames, which can make them slower still.
- **8**, the limit, means the simulation fell behind and time was dropped. The game runs in slow
  motion. Reduce the cost of the fixed update or the `fixed_rate`.
- **Often 0** at a high frame rate is normal. Motion is then interpolated (`fixed_step_alpha`).
- **It follows the previous frame, not its own.** Each frame catches up on the time that passed
  before it, so a long frame is followed by a frame with extra steps, and then often by one with
  none.

## 3. Memory, from the operating system

Sampled `sample_rate` times a second. These see everything in the process: the engine, C
libraries, the graphics driver's system memory, and the code itself.

### Memory resident (MB)

The physical RAM the process occupies now: the working set on Windows, `VmRSS` on Linux. It counts
shared libraries, so it is larger than the game's own data. The operating system can also take
pages away from a process that isn't using them, so it can drop without the game freeing anything,
for example while minimized.

### Memory private (MB)

Memory that belongs to this process alone, which is the better measure of what the game itself
uses and of leaks.

- **Windows:** private bytes, everything the process has committed, whether in RAM or in the page
  file.
- **Linux:** anonymous memory in RAM plus swap (`RssAnon` + `VmSwap`). Memory the game has
  allocated but never touched isn't counted, so it can read lower than on Windows for the same game.

The two systems count differently, so compare runs on the same system.

### How to read memory over time

- **A slow, steady climb that never comes back down** through play that should be repetitive, such
  as loading the same level twice, suggests a leak. Compare the value right after each load.
- **A climb that levels off** is a cache, pool or allocator filling up to its working size.
- **A step up on loading and a step down on unloading** is normal. If the step down is smaller than
  the step up, something from the old level stayed loaded. Assets are freed when nothing refers to
  them any more (ADR 0009), so look for a reference kept by the game.

## 4. The heap

### What is counted

Games that list a profiler are built with a counting version of the global `operator new` and
`operator delete` (`runtime/entry/heap_counting.cpp`). It counts every C++ allocation, on every
thread, in the engine, the game and C++ libraries.

It does **not** count:

- `malloc` from C libraries and drivers, such as SDL, miniaudio or the Vulkan driver. That memory
  is in `Memory private`, but not here.
- Allocations the compiler removed. C++ lets an optimizer drop a `new` and `delete` whose memory is
  never really used, so release builds can show fewer allocations than debug builds for the same
  code. What is counted is what actually reached the allocator.

### Heap allocations per frame and Heap allocated per frame (KB)

How many allocations were made during the frame, and how many kilobytes they asked for. Frees
don't reduce them: they measure churn, not growth.

- **The goal in steady play is zero**, or close to it. Each allocation takes time, can take a lock
  shared with other threads, and fragments memory. A few hundred per frame is a visible cost.
- **Allocations on every frame** usually come from containers created inside a loop or a frame
  (`std::vector`, `std::string`, `std::format`), `std::function` with large captures, or
  `make_shared` for short-lived objects. Find a frame with allocations, then look at its zones
  and at the game's code under them.
- **Spikes on single frames** are expected for loading, spawning and scene changes.
- **The profiler allocates too:** one 48 KB block for every 1024 events it records, and a copy of
  each zone detail longer than about 15 characters. An isolated frame with 1 or 2 allocations of
  48 KB is the profiler, not the game.
- **Every thread counts.** An allocation by the audio thread or a physics worker lands in
  whichever frame was running. A steady trickle the main thread doesn't explain may come from
  another thread.

### Heap in use (MB)

The C++ heap currently allocated, measured by the allocator's own block sizes, so it includes
each block's rounding. Read it like `Memory private`: a climb that never falls suggests a leak.
It is smaller than `Memory private`, which also holds C allocations, code and the driver. When
`Memory private` grows but `Heap in use` doesn't, the growth is outside the C++ heap, such as in a
C library or the graphics driver.

## 5. CPU usage (%)

The CPU time that all of the process's threads used since the previous sample, as a share of the
time that passed. **100% means one core kept fully busy**, so a game with several threads can go
well above 100%. The first sample has nothing to compare with, so there is one sample fewer than
the other sampled values.

- Compare it with `Frame CPU work`: if the main thread's work is small but CPU usage is high, other
  threads, such as physics workers, audio or the driver, are doing the work.
- A game limited by vertical sync or `max_fps` should show low usage. High usage while idle, such as
  in a menu, is wasted power, which matters on laptops.
- **On Windows, single samples are rough.** Windows updates a process's CPU time once per scheduler
  tick, about 15 ms. At 10 samples a second, each sample can be off by about 15 percentage points in
  either direction. The mean over many samples is accurate. For steadier samples, lower
  `sample_rate`, so each one spans more ticks.

## 6. GPU memory

### GPU memory (MB) and GPU memory budget (MB)

How much of the graphics card's own memory the game uses, and how much the system lets it use
before it must start moving data out to system memory. Usage above the budget, or close to it,
causes stutters as textures and buffers are moved, and on Windows the budget shrinks while other
programs use the GPU.

The numbers depend on the backend:

| Backend | `GPU memory` | `GPU memory budget` |
|---|---|---|
| `directx` | Windows' own count of the process's dedicated video memory: every texture, buffer and swapchain, and the driver's objects | What Windows grants the process now |
| `vulkan`, with `VK_EXT_memory_budget` (most desktop drivers) | The driver's count for the program, on the GPU's own heaps, swapchain included | The driver's figure |
| `vulkan`, without it | Only the memory the engine's allocator (VMA) holds, in the GPU's own heaps | VMA's estimate, 80% of the heaps' size |
| `null` | Absent | Absent |

- **Compare runs on the same backend and GPU only.** Two backends count different things.
- **On integrated GPUs** the GPU's memory is system memory, so it also appears in the process's
  memory, and the budget is part of the system RAM.
- The value moves in steps, because memory is taken from the driver in large blocks and buffers
  are sub-allocated from them.

Whether `VK_EXT_memory_budget` was available isn't logged yet. A `GPU memory budget` of exactly 80%
of the card's memory suggests it wasn't.

## 7. The engine's own budgets: Tracked <tag> (MB)

Memory held through Core's `tracked_allocate`, by `MemoryTag`, which `LinearAllocator` and
`PoolAllocator` use. A tag appears only once something has used it, and today the runtime and
samples don't use these allocators, so these values appear only in games that do. Read them like
`Memory private`, per subsystem.

## 8. Engine systems

These are recorded each time the system runs, so they show how much work each frame asked for.
When a frame is slow, check these first, because a jump in work explains a jump in time.

| Value | Meaning | Look for |
|---|---|---|
| `Draw calls` | Model parts drawn by `Renderer::render` | Growth with the number of objects on screen. Each costs CPU time in `Render` |
| `Debug lines` | Lines drawn for debug shapes | Only when debug drawing is on |
| `UI draw commands` | Draw commands of the game interface and debug tools | Jumps when menus open |
| `Physics 3D bodies`, `Physics 2D bodies` | Bodies in the simulation, once per fixed step | Growth that never falls: objects spawned and never removed |
| `Physics 3D contact events`, `Physics 2D contact events` | Contacts that began, continued or ended in the step | Spikes when piles collide. Each is published to listeners, which the game pays for |
| `Audio voices` | Sounds playing | A number that only grows: voices that never end, such as looping sounds never stopped |
| `World transforms` | Entities whose transforms were updated, which is every entity with a `Transform` | Entity count growth |

Values recorded more than once per frame, such as physics with two fixed steps, show each step as
its own sample. Two worlds updated in the same frame share the `World transforms` graph.

## 9. Worked example

The hello game (`samples/hello_game`, null platform) was run for 120 frames with `max_fps = 60`,
in the Linux container where this was written:

```
Profile values (samples: min / mean / p95 / p99 / max):
  CPU usage (%): 19: 0.3336 / 0.4979 / 0.6324 / 0.6324 / 0.6324
  Fixed steps per frame: 120: 0 / 0.9917 / 1 / 1 / 2
  Frame CPU work (ms): 120: 0.004041 / 0.02605 / 0.09819 / 0.1978 / 0.208
  Frame time (ms): 120: 0.006894 / 16.74 / 16.96 / 22.62 / 41.25
  Heap allocated per frame (KB): 119: 0 / 0.4035 / 0 / 0 / 48.02
  Heap allocations per frame: 119: 0 / 0.01681 / 0 / 0 / 2
  Heap in use (MB): 120: 0.05058 / 0.07011 / 0.09746 / 0.09746 / 0.09746
  Memory private (MB): 20: 0.25 / 0.2687 / 0.2969 / 0.2969 / 0.2969
  Memory resident (MB): 20: 3.992 / 4.011 / 4.039 / 4.039 / 4.039
```

Read this way:

- **The frame rate is the limiter's, with room to spare.** Frame time averages 16.74 ms (59.7 FPS)
  while the CPU works 0.03 ms per frame on average: the game spends almost all of each frame
  asleep in `Frame limit`.
- **It stutters, but not because of the game.** Four of the 120 frames took longer than 17 ms:
  17.3, 20.6, 22.6 and 41.3 ms. Frame CPU work never passed 0.21 ms, so the time went elsewhere, and
  the timeline puts it inside `Frame limit`: the operating system woke the game late from its
  sleep. That is common on a busy build machine. On a player's PC it would be worth a second look.
- **p95 and p99 show it.** p95 is 16.96 ms, a normal frame. p99 is 22.6 ms, because of those few
  late frames, and the maximum is the 41.3 ms one.
- **The frame after the hitch took 0.007 ms**, the minimum. The limiter didn't sleep, so as not to
  fall further behind (section 2).
- **Fixed steps** were 1 in 117 frames. The single 2 is the frame after the hitch, catching up on
  its 41 ms, and the 0 that follows is the frame after that (section 2).
- **The heap is quiet.** 118 of 119 frames allocated nothing. The one frame with 2 allocations
  totaling 48 KB is the profiler taking a new block for its events (section 4).
- **Memory is flat.** Resident memory stays at 4 MB, and private memory grows by 0.05 MB, about
  what the profiler's own events take. There is no leak.
- **The profiler's cost is visible.** Each `Profiler sampling` zone took about 0.1 ms, 20 times in
  2 seconds.
- **There is no GPU frame time**, because this run had no graphics device.

## 10. Limits

- **GPU time covers the whole frame only.** There is no breakdown by part of the frame, such as the
  scene against the interface, and GPU work isn't drawn on the timeline beside the CPU's zones.
  ADR 0017 explains why: the timeline needs the GPU's clock matched to the CPU's, which Direct3D 11
  can't do.
- **Android isn't covered**, as agreed: no profiler is listed there, and heap counting is left out
  of Android builds.
- **What was verified, and what wasn't.** The process, heap, frame timing, summary and engine
  values were compiled with GCC and run on Linux, including under AddressSanitizer and
  ThreadSanitizer, and the hello game's profiling check reads them from a real trace. The Windows
  parts of `process_stats.cpp` and `heap_counting.cpp`, and the `memory_usage` and GPU timing code
  of the Vulkan and DirectX backends, could not be compiled there, since neither Windows nor the
  Vulkan SDK was available. The GPU timing logic of both backends was run against a simulated GPU,
  extracted as written, which checked the ring of queries, late and unreliable results, and
  timestamps that wrap around. Build on Windows, run `dream_rhi_directx_tests`, which measures GPU
  time on WARP, and run a game with each graphics backend to confirm them.
