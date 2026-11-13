# 0018. Memory budgets, and running out of memory

- Status: Accepted
- Date: 2026-11-12

## Context

A game that asks for more memory than the system can give crashes. The engine is built without
exceptions (ADR 0002), so a failed `new` raises `std::bad_alloc` from the standard library, nothing
catches it, and the program ends with no word about why. `Game::on_low_memory` existed, but only
Android's low-memory event called it, so on PC a game heard nothing before the end.

How memory runs out differs by system:

- **Windows** fails allocations once the system's commit limit is reached: RAM plus the page file.
  `new` then really fails.
- **Linux** hands out memory it may not have, by default, so allocations almost never fail. When
  RAM and swap run out, the kernel's out-of-memory killer ends a process instead, with no failure
  a program could see.

So waiting for a failure can't be the whole answer. The game has to hear about shortage early
enough to free memory, and the failure that still happens has to be survivable.

[docs/reports/memory-budgets.md](../reports/memory-budgets.md) describes the strategy in detail
and how a game uses it.

## Decision

### Pressure, decided from budgets

`runtime::MemoryBudget` turns readings into a `MemoryPressure` of `Normal`, `Warning` or
`Critical`:

| Level | When |
|---|---|
| Critical | The process's private memory is over `budget_mb`, the system's room to allocate is below `critical_system_mb`, or an allocation failed |
| Warning | The process has used `warning_at` of its budget, or the system's free RAM is below `low_system_mb` |

The game's own budget is optional. Without one, only the system's figures count. To leave a level,
a reading must clear its threshold by 10%, so the pressure doesn't flicker.

The room to allocate is Windows' available commit for the process. On Linux it is available RAM
plus free swap, capped by the commit limit when the system doesn't overcommit. Core reads them
with `core::system_memory`, next to `core::process_memory` (ADR 0016).

### Reacting

The App checks `check_rate` times a second, and once before `Game::on_start`. When the pressure
changes, it:

1. publishes `MemoryPressureChanged` on the event bus;
2. calls `Game::on_low_memory` when the pressure rose, once the game has started;
3. makes the AssetManager refuse new assets while critical, with `ErrorCode::OutOfMemory`, so a
   load fails cleanly instead of taking the last memory. Assets already loaded are still handed
   out, and a scene that can't load leaves the current one untouched (ADR 0009).

`App::memory_pressure` gives the level at any time, so `on_start` can choose lighter content.

### The emergency reserve

`core::MemoryReserve` keeps `reserve_mb` aside and is the program's new-handler, which `operator
new` calls when it can't allocate. The first failure frees the reserve, and the allocation tries
again and succeeds. The App sees the failure at its next frame, at once rather than at the next
scheduled check, and the pressure is critical. Once the readings are normal again, with their
margin, the App takes the reserve back, which also proves the memory is there, and the pressure
eases.

A failure with the reserve already spent is written to stderr, without allocating, and the handler
steps aside, so the standard behaviour follows: `new (std::nothrow)` returns null, and any other
`new` ends the program, now with that report instead of a bare crash.

The reserve is taken with `malloc`, so the handler can free it while an allocation is failing, on
any thread, without running destructors.

### Settings

```toml
[memory]
enabled = true             # Off by default on Android, whose phones keep little RAM free.
budget_mb = 0              # The game's limit on its private memory. 0: none.
warning_at = 0.85
low_system_mb = 1024
critical_system_mb = 256
reserve_mb = 32
check_rate = 4
refuse_loads = true
```

The profiler records `Memory pressure`, `System RAM available (MB)`, `System commit available
(MB)` and, with a budget, `Memory budget used (%)` at each check.

## Consequences

- A game on PC hears about low memory through the same `on_low_memory` it already had, and can
  free caches, unload what the current scene doesn't need, or lower its quality.
- A single allocation failure no longer ends the program. `dream_runtime_memory_exhaustion_test`
  runs a game out of memory under a real limit set on its own process, an address-space limit on
  Linux and a job's memory limit on Windows. With the guard it survived 16 failures in 200 frames.
  With the guard turned off, the same run ended in `std::bad_alloc`.
- The engine can't free memory for the game. A game that ignores `on_low_memory` and keeps
  allocating still runs out, just later, and with a clear report.
- A single allocation larger than the reserve still fails after it. The reserve covers the many
  small allocations that keep a frame going. Large ones are mostly assets, which is why loads are
  refused while critical.
- On Linux with the usual overcommit, the out-of-memory killer can still end the game if memory
  runs out between checks, or if the game doesn't free enough. The thresholds are what protect it
  there, so a higher `critical_system_mb` gives more warning.
- Limits that the system's figures don't show, such as a container's memory limit, are caught
  only by the reserve.
- The reserve is memory the game can't use: 32 MB of committed memory on Windows. On Linux it is
  address space, which costs no RAM until touched, and it helps only where allocations can fail,
  such as under an address-space limit or without overcommit.
- Video memory is not part of the pressure. Failed GPU allocations already return errors from the
  graphics interface.
