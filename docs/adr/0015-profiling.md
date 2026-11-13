# 0015. Profiling

- Status: Accepted, extended by [0016](0016-profiling-resources.md), which adds frame timing,
  memory and counters
- Date: 2026-11-12

## Context

Games need to see where their frame time goes, first on PC: Windows and Linux. Profiling cuts
across the layers. Zones are worth placing in Core's job system, the runtime's main loop and
systems, backends and games alike, yet the tool that records them should be swappable through
configuration like any other backend (ADR 0001). Third-party code must be under the MIT or
Apache 2.0 license (ADR 0014).

## Decision

### Instrumentation lives in Core

`dream/core/profile.h` is the vocabulary every layer uses, as logging is:

```cpp
DREAM_PROFILE_ZONE("Physics step");              // Measures the rest of the scope.
DREAM_PROFILE_ZONE_DETAIL("Load scene", path);   // The same, describing this occurrence.
DREAM_PROFILE_VALUE("Bodies", body_count);       // A number drawn as a graph over time.
DREAM_PROFILE_MESSAGE("Level {} loaded", level); // A moment, formatted like DREAM_LOG_INFO.
DREAM_PROFILE_THREAD("Loader");                  // Names the calling thread.
DREAM_PROFILE_FRAME();                           // The main loop already does this.
```

The macros send events to one installed `core::ProfileSink`, as log messages go to a `LogSink`.
With none installed, each costs an atomic load and a branch. The CMake option `DREAM_PROFILING=OFF`
defines `DREAM_DISABLE_PROFILING`, and the macros then compile to nothing, without evaluating their
arguments.

Zone names must be string literals. Each zone's name, file and line form a static constant, so a
backend can keep pointers instead of copying strings, and every occurrence of a zone groups
together. Text that differs per occurrence, such as a file name, goes in the detail.

A zone ends on the sink it began with, so a sink must outlive every zone that began while it was
installed.

### The profiler interface

`profiler::Profiler` derives from `core::ProfileSink` and adds `flush`, which saves what has been
recorded. A project chooses a backend like any other:

```toml
[modules.windows]
profiler = ["timeline"]
```

`runtime::ProfilerSession` installs it. `run_program` starts the session before the platform,
graphics device and game exist, and ends it after they are gone, so the threads those systems own
have finished their zones. Ending the session flushes the profiler. A project without a `profiler`
list has no profiler, and the macros stay at their idle cost.

### Backends

| Backend | What it does |
|---|---|
| `timeline` | Records in memory, and on flush writes a JSON file in the Chrome trace event format, which https://ui.perfetto.dev, chrome://tracing and Speedscope open |
| `null` | Records nothing |

Settings for `timeline`, all optional:

```toml
[profiler.timeline]
path = "trace.json"    # Relative to the working directory. Missing folders are created.
max_events = 1000000   # About 48 bytes each. Later events are dropped and counted.
start_frame = 0        # Frames to skip before recording, such as while loading.
frames = 0             # How many frames to record. 0 means until the end.
```

The trace is named after `title` under `[app]`. The file is written beside its final name and then
renamed, so a failed write never leaves half a trace.

### Why an in-house library

- Tracy, the usual choice for games, is under the BSD 3-Clause license, which ADR 0014 excludes.
- The Perfetto SDK is Apache 2.0, but it is a large dependency, and its system-wide tracing works
  only on Linux and Android. It is a candidate for a second backend when traces need to include the
  operating system's scheduling.
- The Chrome trace format is documented, plain JSON, and opened by several free viewers, so a small
  library covers it.

`libraries/timeline` records and writes the format, using only the standard library, and the
`timeline` backend adapts it to the interface, as ADR 0014 arranges for in-house libraries. Each
thread records into its own buffer without locking. Buffers grow in blocks of 1024 events, taking a
lock once per block, and each event is published with release ordering, so a trace can be written
while other threads record. The `max_events` budget is handed out in whole blocks. Zones are stored
as they end and sorted by start when written, since viewers expect a parent before its children.
Sources are written as file names without folders, which keeps traces smaller.

### Zones the engine provides

Every game gets these without writing any profiling code:

| Where | Zones and values |
|---|---|
| Main loop | `Start` (window and `Game::on_start`), `Platform events`, `Frame`, and inside it `Queued events`, `Fixed update`, `Update`, `Begin GPU frame`, `Render`, `Submit and present` and `Frame limit`; then `Stop` |
| Runtime systems | `Physics 3D step` and `Physics 3D simulate`, the same for 2D, `Draw scene`, `Audio update`, `Interface update`, `Update transforms`; values `Physics 3D bodies`, `Physics 2D bodies` and `Draw calls` |
| Loading | `Load scene` and `Load asset`, with the path as detail |
| Job system | `Job` around each job, and worker threads named `Job worker <n>` |

A frame is marked after its `Frame` zone ends, so a profiler recording a range of frames gets each
one whole. `Begin GPU frame` is where the CPU waits for the GPU or the display.

## Consequences

- Any game can be profiled on PC by adding `profiler = ["timeline"]` to its module list, with no
  code changes. The arcade demo does so on Windows and Linux, and `dream_hello_game_profiler` runs
  the hello game with it and checks the trace it saves.
- Like `platform` and `debug_ui`, the interface has one real backend beside `null`, short of
  ADR 0001's rule 3. The Perfetto SDK is the natural second one.
- The trace is saved when the program ends, or when `ProfilerSession::flush` is called. A crash
  loses it.
- Only CPU time is measured. GPU timing would need timestamp queries in the graphics interface.
- Work inside third-party libraries, such as Jolt's worker threads, shows only as the runtime's
  zone around it.
- Android was not covered at first. Since Phase 10, the game's working directory there is the
  app's files folder, so `timeline` saves its trace there, but no Android module list includes a
  profiler by default. Heap counting is still left out of Android builds.
