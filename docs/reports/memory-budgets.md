# Report: memory budgets, and keeping a game alive when memory runs out

## Summary

Until now, a game that asked for more memory than the system had crashed: a failed allocation
raised `std::bad_alloc`, which nothing catches in an engine built without exceptions, and the
program ended without saying why. On PC the game also had no warning, since `Game::on_low_memory`
was only called by Android.

The engine now defends the game in four layers:

| Layer | What it does | Protects against |
|---|---|---|
| 1. Watch | Checks the game's memory against its budget, and the system's remaining memory, a few times a second | Running out without warning |
| 2. Warn | Tells the game when the pressure rises, through `on_low_memory` and an event, so it can free memory | Memory use growing until it fails |
| 3. Contain | Refuses new assets while the pressure is critical, with an error the game can handle | The large allocations most likely to fail |
| 4. Survive | Keeps an emergency reserve, given back when an allocation fails, so the allocation succeeds and the game carries on | The failure that happens anyway |

If the reserve is gone too and another allocation fails, the engine reports it clearly on stderr
before the program ends, instead of a bare crash.

It works without any setup: every game on PC gets it with the defaults. A game can also set its own
memory budget.

Tested under a real memory limit, a game allocating 8 MB every frame with no budget set survived 16
allocation failures in 200 frames. The same game with the guard turned off crashed with
`std::bad_alloc` on the first one.

The decision is recorded in [ADR 0018](../adr/0018-memory-budgets.md).

## 1. Why games crash when memory runs out

When `new` can't get memory, C++ calls the new-handler, if one is installed, and otherwise throws
`std::bad_alloc`. The engine is compiled without exceptions (ADR 0002), so nothing catches it, and
the program is ended. Before this change there was no new-handler, so any failed allocation ended
the game.

When that happens depends on the system:

- **Windows** lets a program commit memory up to the system's commit limit, which is RAM plus the
  page file. Past it, allocations fail, and `new` fails with them. Long before that, when RAM is
  full, Windows starts paging, which shows up as stutter.
- **Linux**, by default, overcommits: it hands out memory it may not have, and only finds it when
  the memory is first used. Allocations almost never fail. When RAM and swap run out, the kernel's
  out-of-memory killer ends a process outright, and there is no failure for the program to catch.

So on Linux, catching failures alone would protect nothing. The engine has to warn the game early,
while it can still free memory, and that warning is the main protection on both systems. The
reserve is for the failures that happen anyway, which is mostly a Windows concern.

## 2. The strategy

### 2.1 Watching: what is measured

At each check, the App reads:

| Reading | Windows | Linux |
|---|---|---|
| The game's private memory | Private bytes: everything it committed | Anonymous memory in RAM plus swap |
| The system's free RAM | Available physical memory | `MemAvailable` |
| The system's room to allocate | Available commit, for this process | `MemAvailable` plus free swap, capped by the commit limit if the system doesn't overcommit |
| Whether an allocation failed | From the emergency reserve | The same |

It decides a pressure level from them:

| Level | Raised when | The game should |
|---|---|---|
| `Normal` | Nothing below applies | Carry on |
| `Warning` | The game has used `warning_at` (85%) of its budget, or the system's free RAM is under `low_system_mb` (1024 MB) | Free what is cheap to rebuild, such as caches, and avoid growing |
| `Critical` | The game is over its budget, the system's room to allocate is under `critical_system_mb` (256 MB), or an allocation failed | Free memory now. New assets are refused meanwhile |

The budget is optional and off by default. Without it, the game is judged by the system's figures
only, which suits a game that doesn't know what machine it will run on. With it, a game holds
itself to a limit its designers chose, such as the memory of the smallest machine it supports.

To go back down a level, a reading must clear the threshold by 10%. A game at 1000 MB of a 1000 MB
budget stays critical until it is under 900 MB, and stays at a warning until it is under 765 MB
(90% of the warning's 850 MB). This stops the level flickering, and the game being told over and
over, when memory hovers around a threshold.

Checks run `check_rate` times a second (4), in the main loop at the start of each frame, before
the game updates. One more runs before `Game::on_start`, so a game can read the level there and
load lighter content. A failed allocation skips the wait: the App checks at the very next frame.

### 2.2 Telling the game

When the level changes, the App:

1. logs it, with the figures behind it;
2. publishes `MemoryPressureChanged` on its event bus, with the new and previous levels and the
   reading;
3. calls `Game::on_low_memory` if the level rose, which is the same call Android's low-memory event
   makes;
4. turns asset refusal on or off (section 2.3).

The game can also ask at any time with `App::memory_pressure()`.

### 2.3 Containing: refusing new assets while critical

Loading is where a game takes the most memory at once. While the level is critical, the
AssetManager refuses to load an asset that isn't already in memory, and `load` returns an error
with `ErrorCode::OutOfMemory`. The game handles it like any other load error, which it must already
do for missing files. Assets already loaded are still returned, so the game keeps working with what
it has.

Scenes load through the AssetManager, so a scene that needs a refused asset fails to load as a
whole, and the current scene stays as it was (ADR 0009). `[memory] refuse_loads = false` turns
this off, for a game that would rather try.

### 2.4 Surviving: the emergency reserve

When the engine starts a game, it puts `reserve_mb` (32 MB) aside and installs itself as the
new-handler. When an allocation then fails:

1. `operator new` finds no memory, and calls the new-handler.
2. The handler frees the reserve and returns.
3. `operator new` tries again, and this time there is memory, so the allocation succeeds. The code
   that asked for it never knows it failed.
4. At the next frame, the App sees the failure, sets the level to critical, and tells the game
   (section 2.2). The game frees memory.
5. At a later check, once the other readings are normal with their 10% margin, the App takes the
   reserve back. Getting it is the proof that memory is available again, and the level returns to
   normal. The next failure is covered the same way.

The handler runs inside a failing allocation, on whatever thread that was, so it does nothing that
could allocate: the reserve is taken with `malloc` and given back with `free`.

A failure is always reported to the game at least once, even if memory looks fine by the next
check, because something did run out.

### 2.5 When nothing is left

If another allocation fails while the reserve is spent, the handler writes a report to stderr. It
formats the text on the stack and writes it directly, without allocating:

```
[FATAL] Out of memory: an allocation failed and the memory reserve is already spent (2 failed
allocation attempts; C++ heap in use 742 MB). The program ends unless the allocation was allowed to
fail. See docs/reports/memory-budgets.md.
```

The heap figure appears in games that list a profiler, which count their allocations (ADR 0016).
Then the handler steps aside, and C++'s standard behaviour follows: `new (std::nothrow)` returns
null, and any other `new` ends the program. The game still stops, but with a clear reason in its
output, rather than a crash with no explanation.

## 3. What the game has to do

The engine can tell the game memory is short, and buy it time, but it can't know what the game
can do without. The game's part:

```cpp
class MyGame final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        // Under pressure from the start: choose lighter content.
        m_high_detail = app.memory_pressure() == runtime::MemoryPressure::Normal;
        return load_level(app);
    }

    void on_low_memory(runtime::App& app) override {
        // Free what can be rebuilt. Assets are freed as soon as nothing refers to them.
        m_decal_cache.clear();
        m_far_chunks.clear();
        if (app.memory_pressure() == runtime::MemoryPressure::Critical) {
            m_high_detail = false;
        }
    }
};
```

- **Free on `on_low_memory`.** Caches, pooled objects, parts of the world far from the player,
  sounds that aren't playing, high-detail versions of assets. Assets are released once the game
  holds no reference to them (ADR 0009), so dropping references is what frees them.
- **Handle load errors.** While critical, `load` fails with `ErrorCode::OutOfMemory`. Skip the
  optional asset, use a placeholder, or tell the player, rather than treating it as fatal.
- **Don't grow while critical.** Spawning, streaming in and filling caches can wait for the
  `MemoryPressureChanged` event that says the pressure went back down.

A game that ignores all of this still benefits from the reserve and the clear report, but it will
run out again.

## 4. Settings

All optional, under `[memory]` in the project configuration:

| Setting | Default | Meaning and advice |
|---|---|---|
| `enabled` | `true`, `false` on Android | Turns all of this off. Android phones keep little RAM free by design, and report low memory themselves |
| `budget_mb` | `0`, no budget | The most private memory the game means to use. A good value: the highest `Memory private (MB)` of a long play session in the profiler, plus a margin, and no more than the smallest supported machine allows |
| `warning_at` | `0.85` | The share of the budget that raises a warning. Lower it for more time to react |
| `low_system_mb` | `1024` | A warning when the system's free RAM falls below this. Paging, and stutter, are near |
| `critical_system_mb` | `256` | Critical when the system's room to allocate falls below this. On Linux, where the out-of-memory killer gives no failure to catch, a higher value gives more warning |
| `reserve_mb` | `32` | The emergency reserve. It should cover everything a frame allocates, so the game can reach its next frame. 0 turns the reserve off but keeps the report |
| `check_rate` | `4` | Checks per second. Each costs a fraction of a millisecond |
| `refuse_loads` | `true` | Whether new assets are refused while critical |

```toml
[memory]
budget_mb = 2048
low_system_mb = 1536
```

## 5. Platform notes

- **Windows.** All four layers apply. Allocations fail at the commit limit, so the reserve matters
  here, and the system's room to allocate is Windows' own figure for this process. The reserve
  commits 32 MB, which the game can't use.
- **Linux.** Watching and warning are the real protection, since overcommit means allocations
  rarely fail and the out-of-memory killer gives no notice. The reserve still helps where
  allocations can fail: under an address-space limit (`ulimit -v`) or with overcommit turned off.
  On Linux the reserve is only address space until used, so it costs no RAM.
- **Android.** Off by default, as agreed to focus on PC. Android's own low-memory event still calls
  `on_low_memory`.
- **Containers and other outside limits**, such as a Linux cgroup's memory limit or a Windows job
  object, don't show in the system's figures. The reserve is what covers them. This is also how the
  test below limits itself.

## 6. Seeing it in a profile

With a profiler listed, each check records these values, alongside those in
[reading profiles](reading-profiles.md):

| Value | Meaning |
|---|---|
| `Memory pressure` | 0 normal, 1 warning, 2 critical |
| `System RAM available (MB)` | The system's free RAM, compared with `low_system_mb` |
| `System commit available (MB)` | The system's room to allocate, compared with `critical_system_mb` |
| `Memory budget used (%)` | The game's private memory as a share of `budget_mb`, when there is one |

A `Memory check` zone shows when each check ran, and what it cost. The log also records every
change of level, with its figures.

## 7. How it was verified

- **The decision logic** (`memory_budget_test.cpp`): every threshold in both directions, the 10%
  margins, failures, thresholds turned off, and every setting, valid and invalid.
- **The reserve** (`memory_reserve_test.cpp`): it installs and removes itself, gives the reserve
  back and refills, reports and steps aside once spent, and a real `new (std::nothrow)` far too
  large for any machine returns null instead of ending the program.
- **The App** (`memory_budget_test.cpp`):
  - a budget the game is over is known in `on_start`, and new assets are refused;
  - a failed allocation during a frame makes the next frame critical, calls `on_low_memory` once
    and refuses loads, then the next check refills the reserve and returns to normal;
  - turning it off leaves no handler behind.
- **A real run out of memory** (`dream_runtime_memory_exhaustion_test`). The test limits its own
  process: the address space on Linux, a job's memory limit on Windows. It gives the game 160 MB
  more than it has at the start, and runs a game that allocates 8 MB every frame, with no budget
  and no system thresholds. It checks that all 200 frames run, that the game was told, and that it
  never held more than the limit. On Linux, the game survived 16 allocation failures. Run with the
  guard turned off, the same program ended with `terminate called after throwing an instance of
  'std::bad_alloc'`.

All of this was compiled with GCC and run on Linux, also under AddressSanitizer and
ThreadSanitizer. The Windows parts could not be compiled there: `GlobalMemoryStatusEx` in
`process_stats.cpp`, and the job object in the exhaustion test. Running `dream_runtime_tests` and
`dream_runtime_memory_exhaustion_test` on Windows confirms them.

## 8. Limits

- **The engine can't free the game's memory.** The game has to act on `on_low_memory`. One that
  keeps allocating runs out again, only later and with a clear report.
- **A single allocation larger than the reserve still fails** once the reserve is given back. The
  reserve covers the small allocations that keep a frame going. Large allocations are mostly assets,
  which is why they are refused while critical.
- **On Linux, the out-of-memory killer can still end the game** if memory runs out between two
  checks, or the game doesn't free enough. A higher `critical_system_mb` gives more warning.
- **Video memory is not part of the pressure.** The graphics backends already return errors when
  the GPU's memory runs out, and `GPU memory (MB)` against `GPU memory budget (MB)` shows it in a
  profile.
- **Allocations made with `malloc`** by C libraries and drivers don't go through the new-handler. On
  Windows a failed `malloc` in a library is up to that library to handle. The warning layers still
  see that memory, because they read the whole process.
