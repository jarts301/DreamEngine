# 0019. Crash reports

- Status: Accepted
- Date: 2026-11-12

## Context

When a game crashed, nothing was left behind but the operating system's message: no reason, no
place in the code, no hint of what the game was doing. A crash on a player's machine, or one that
happens rarely, couldn't be understood afterwards. Games are played mostly on Windows, and
developed on Windows and Linux.

Writing anything after a crash is delicate. The heap may be corrupt, a lock may be held by the
thread that crashed, and after a stack overflow that thread has almost no stack left.

## Decision

### What is caught

`core::CrashHandler` catches every way a process can end badly that a program can see:

| Crash | Windows | Linux |
|---|---|---|
| Invalid memory access, division by zero, illegal instruction | Unhandled exception filter | `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL` |
| Stack overflow | The same, with a writer thread | `SIGSEGV`, on an alternate signal stack |
| A failed `DREAM_VERIFY` or `DREAM_ASSERT` | It stops at a breakpoint | It stops at a trap, `SIGILL` |
| `abort()` | `SIGABRT` | `SIGABRT` |
| `std::terminate` | Terminate handler | Terminate handler, which names the exception type |
| Pure virtual call, invalid C runtime parameter | Their runtime handlers | Through `std::terminate` |

After writing the report, the crash carries on as it would have. On Linux the previous handler is
put back and the signal raised again, so the exit status and any core dump are unchanged. On
Windows the process ends with the exception's code, without Windows Error Reporting's dialog,
whose own report this replaces.

### What a report holds

A text file, `crash-<date>-<time>-<process>.txt`, with the reason and address, the application,
engine version, time, process and thread, the frame the main loop was on, details that the engine
records as it starts (the module lists in use and every backend created), memory, the last 32 log
lines, and the crashed thread's stack. On Windows, a minidump (`.dmp`) sits beside it, which
Visual Studio or WinDbg opens at the crash with every thread's stack.

### Writing without allocating

Every part of the report is written with fixed buffers and the system's own file calls, never the
heap, the log or `std::format`. Details and log lines are copied into fixed storage while the
program runs, so after a crash they are only read.

- **Linux** writes from the signal handler, on a 64 KB stack of its own, set up for the thread
  that installs the handler. The stack trace comes from `backtrace_symbols_fd`, which writes
  without allocating, as program offsets that `addr2line` resolves.
- **Windows** starts a writer thread when the handler is installed. The crashed thread only
  records its registers, wakes the writer and waits, so a stack overflow can still be reported.
  The writer walks the stack with DbgHelp, naming functions and lines from `.pdb` files, and
  writes the minidump. The text that needs no symbols is written first, in case DbgHelp fails.
- One report per process: a second crash, or a crash while reporting, doesn't start another.

### In games

`run_program` installs the handler first, from `[crash_report]` in the project configuration, and
removes it last. `App` records the frame number every frame, and `App::create_module` records
each backend it creates.

```toml
[crash_report]
enabled = true               # Off by default on Android.
directory = "crash_reports"  # Relative to the folder the game is started from.
minidump = true              # Windows only.
simulate = "none"            # Or "null_pointer", "divide_by_zero", "stack_overflow", "abort",
                             # "assert" or "terminate", to try the reports.
simulate_after_frames = 300
```

`simulate` crashes the game on purpose, in the frame after `simulate_after_frames` frames, through
`core::simulate_crash`, so the reports can be tried without a real bug.

## Consequences

- Every crash leaves a report saying why, where, and what the game was doing. The reasons are
  explained in plain words, such as a failed `DREAM_VERIFY` being the cause of a breakpoint, and
  the log shows the assertion's message.
- `dream_crash_report_*` tests crash a probe program in each of the six ways and check its report,
  and on Windows its minidump. `dream_hello_game_crash` does the same with a game that crashes
  through `[crash_report] simulate`.
- Function names in Windows reports need `.pdb` files beside the program, which the Debug and
  RelWithDebInfo builds make. The Release preset makes none, so its reports name modules and
  offsets, and its minidumps need the matching `.pdb` files to be read.
- Linux reports give offsets, not names, because naming them would need the program's symbols
  exported, and `-rdynamic` would let volk's Vulkan function pointers take the place of the Vulkan
  loader's own functions.
- A stack overflow on a thread other than the installing one is reported on Windows, but on Linux
  only the installing thread has the alternate stack, so the process ends without a report.
- Some failures can't be caught by any handler in the process: `__fastfail` on Windows, which the
  heap uses for some corruption, being killed by the system, such as by Linux's out-of-memory
  killer, and power loss.
- While installed, the handler replaces any other unhandled exception filter on Windows.
- Android is not covered yet.
