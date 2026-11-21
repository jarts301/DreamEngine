# Report: crash reports, and how to read them

## Summary

When a game or the engine crashes on Windows, Linux or Android, a report is written saying why it
crashed, where, and what the game was doing. On Windows a minidump is written beside it, which
Visual Studio opens at the crash. Every game gets this without any code. The design is recorded in
[ADR 0019](../adr/0019-crash-reports.md).

| Where | What |
|---|---|
| `crash_reports/crash-<date>-<time>-<process>.txt` | The report: reason, address, frame, modules, memory, the last 32 log lines, the stack |
| `crash_reports/crash-<date>-<time>-<process>.dmp` | Windows only: the minidump, with every thread's stack |
| The console | `[FATAL] The program crashed: <reason>` and the report's path |

The folder is relative to where the game was started, and is set with `[crash_report] directory`.

## 1. Trying it

The arcade demo has the option in its `dream.toml`:

```toml
[crash_report]
simulate = "none"
simulate_after_frames = 300
```

Set `simulate` to one of these, rebuild, and run the game. About 5 seconds in, after 300 frames at
60 FPS, it crashes that way and writes a report:

| `simulate` | Crashes with | The report's reason, on Windows / Linux and Android |
|---|---|---|
| `"null_pointer"` | A write through a null pointer | Access violation, writing address 0x0 / Segmentation fault (SIGSEGV) |
| `"divide_by_zero"` | An integer division by zero | Integer division by zero / Arithmetic error (SIGFPE); abort() on ARM, which doesn't trap it |
| `"stack_overflow"` | Endless recursion | Stack overflow / SIGSEGV, "most likely a stack overflow" |
| `"abort"` | `abort()` | abort() was called |
| `"assert"` | A failed `DREAM_VERIFY` | Breakpoint / Illegal instruction (SIGILL), Trap (SIGTRAP) on ARM, with the assertion's message in the log |
| `"terminate"` | `std::terminate()` | std::terminate was called |

Set it back to `"none"` afterwards. Any game can use the same setting, and code can crash on
purpose with `core::simulate_crash`.

## 2. Reading a report

This is the report the hello game writes when its test makes it crash in frame 2, on Linux:

```
=== Dream Engine crash report ===
Application: Dream Hello
Engine: 0.1.0
Time: 2026-11-13 04:37:18 UTC
Process: 9267, thread 9267
Reason: Segmentation fault (SIGSEGV): memory that doesn't exist was accessed, at address 0x0000000000000000
Frame: 2

Details:
  Module lists: [modules.linux]
  platform module: null

Memory:
  VmHWM:	    4516 kB
  VmRSS:	    4516 kB
  RssAnon:	     244 kB
  VmSwap:	       0 kB

Recent log, oldest first:
  [INFO] Using module 'null' for interface 'platform'
  [WARNING] Crashing on purpose, with 'null_pointer', as [crash_report] simulate asks

Stack trace of the crashing thread, innermost first. ...
dream_hello_game_crash(+0x3ecc8)[0x5617f40f3cc8]
dream_hello_game_crash(+0x3ef6d)[0x5617f40f3f6d]
/lib/x86_64-linux-gnu/libc.so.6(+0x45330)[0x7fc724645330]
dream_hello_game_crash(+0x71bb)[0x5617f40bc1bb]
dream_hello_game_crash(+0x27c3b)[0x5617f40dcc3b]
dream_hello_game_crash(+0x29a4b)[0x5617f40dea4b]
...
```

Read it top to bottom:

1. **Reason** says what went wrong. The address tells what kind of bug it is:
   - `0x0` or another small address: a null pointer, or a field of an object through a null
     pointer.
   - An address close to the stack, which the report points out on Linux: a stack overflow.
   - A large address that is neither: a pointer to memory already freed, or never set.
2. **Frame** is the main loop's frame number, counted from 0 as the profiler counts them. "none"
   means the crash happened while starting, before the first frame, such as in `on_start`.
3. **Details** list the module lists used and each backend created, such as `rhi module: vulkan`
   or `audio module: miniaudio`. A crash inside one backend but not another points at the backend.
4. **Memory** shows whether memory was running out. Compare it with the budget in
   [Memory budgets](memory-budgets.md). The C++ heap line appears in games that list a profiler.
5. **Recent log** is often the best clue. A failed assertion's message is its last line, with the
   file and line of the check. Warnings just before the crash often name the cause.
6. **Stack trace** is where the crash happened, innermost first. The first frames are the crash
   handler and the system, so look for the first frame in the game's or the engine's code.

## 3. Getting function names from the stack

### Windows

With `.pdb` files beside the program, which Debug and RelWithDebInfo builds make, the report names
every function, file and line:

```
  #03 0x00007ff6a1b2c3d4 dream_arcade_demo.exe!dream::core::simulate_crash+45 (core\src\crash_report.cpp:453)
```

Without them, as in the Release preset's builds, frames show the module and an offset. Either way,
the minidump shows more: open the `.dmp` file in Visual Studio, choose **Debug with Native Only**,
and it stops at the crash, with every thread's stack and the local variables kept in the dump.
Visual Studio needs the `.pdb` files of the exact build that crashed, so keep them for builds you
give out.

### Linux

Frames are the program or library and an offset. `addr2line` turns an offset into a function and
line, for a program built with debug information, as Debug builds are:

```bash
addr2line -f -C -e dream_hello_game_crash 0x71bb 0x27c3b 0x29a4b
```

For the report above, this gives:

```
dream::core::simulate_crash(dream::core::CrashKind)  core/src/crash_report.cpp:453
dream::runtime::App::run(dream::runtime::Game&)       runtime/src/app.cpp:244
dream::runtime::run_program()                         runtime/src/program.cpp:92
```

Use the binary that crashed, from the same build. Lines for libraries such as `libc.so.6` can be
ignored unless the crash is inside one.

### Android

Reports go to `crash_reports/` in the app's files folder, and logcat says where, under the tag
`DreamEngine`. A debug build's report can be copied off the phone with:

```sh
adb exec-out run-as org.dreamengine.hello cat files/crash_reports/crash-<date>-<time>-<process>.txt > crash.txt
```

Frames look like Linux ones. Those in the game are in `libmain.so`, which the NDK's
`llvm-addr2line` names from the unstripped copy the same build left in
`build/<preset>/samples/<game>/dream_<game>_native/arm64-v8a/`:

```bash
llvm-addr2line -f -C -p -e libmain.so 0x2efdc 0x2eeb8 0x32648
```

The first frames are the crash handler itself, then `__kernel_rt_sigreturn`, then the code that
crashed. Libraries the system owns, such as `libc.so`, are named by their exported symbols.

After the report, the crash goes on to Android's own handler, so the system's tombstone and the
"keeps stopping" dialog still appear, as they would without the engine's handler.

## 4. Settings

All optional, under `[crash_report]`:

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Turns crash reports off |
| `directory` | `"crash_reports"` | Where reports go, relative to the folder the game is started from. Created if missing |
| `minidump` | `true` | Windows: a minidump beside each report. About a megabyte, with the stacks and memory they point to |
| `simulate` | `"none"` | Crashes the game on purpose, to try the reports (section 1) |
| `simulate_after_frames` | `300` | The crash happens in the frame after this many |

Uploading has its own `[crash_upload]` table, below.

If the folder can't be created, the game still runs, without crash reports, and the log says so.

### Uploading reports

With a `crash_upload` module listed, the next start of the game uploads the reports earlier runs
saved, in the background, and moves each into a `sent` folder once its service has it
([ADR 0043](../adr/0043-crash-uploads-and-texture-cookers.md)). Windows and Android only.

```toml
[modules.windows]
crash_upload = ["sentry", "null"]      # Or "backtrace". Without a setup, falls back to null.

[modules.android]
crash_upload = ["sentry", "null"]      # Also makes the APK ask for INTERNET.

[crash_upload]
enabled = true                          # All optional.
max_reports = 5                         # Uploaded per start, oldest first.

[crash_upload.sentry]
dsn = "https://<public key>@<host>/<project id>"   # The project's Client Keys page.
environment = "production"                          # Optional.

[crash_upload.backtrace]
url = "https://submit.backtrace.io/<universe>/<token>"   # The project's submission URL.
```

- **Sentry** gets Windows minidumps, which it symbolicates with the game's PDB files once they are
  uploaded to the project, and Android reports as fatal events. Each has its text report attached.
- **Backtrace** gets minidumps the same way, and Android reports as JSON crashes, the report as an
  annotation.
- A report that fails, usually for the network, stays, and is tried at the next start.
- **Ask players first** where the law requires it: reports list the device's modules and memory,
  and the log's last lines. List a module that sends only in builds where players agreed.

## 5. How it works

- **Nothing is allocated after a crash.** The heap may be what broke, so the report is written
  with fixed buffers and the system's own file calls. Details and the last 32 log lines are copied
  into fixed storage while the game runs, and only read after a crash.
- **On Linux and Android**, the signal handler writes the report on a stack of its own, so a stack
  overflow is reported too, then restores the previous handler and raises the signal again. The
  process ends as it would have, with the same exit status and any core dump.
- **On Windows**, a writer thread waits from the start. The crashed thread records its registers,
  wakes the writer and waits for it, which works even after a stack overflow. The writer writes
  the text, walks the stack with DbgHelp, and writes the minidump. A crash without an exception,
  such as `abort()`, gets a made-up one in the minidump, so the debugger still opens it at the
  right thread and line. The process then ends with the exception's code, without Windows Error
  Reporting's dialog.
- **One report per crash.** If another thread crashes while the report is written, or the crash
  handler itself fails, no second report starts.
- **The crash handler is installed first and removed last**, so crashes while starting and
  shutting down are reported.

## 6. How it was verified

- **Unit tests** (`crash_handler_test.cpp`, `crash_reports_test.cpp`):
  - installing and removing the handler puts back what was there;
  - a second handler, or a folder that can't be made, is refused;
  - the date conversion is checked against known dates, including a leap day and a time before
    1970;
  - the report writer, the file names, and every setting and its errors;
  - the App refuses an unknown `simulate`, and doesn't crash before its frame.
- **Real crashes**: `dream_crash_report_null_pointer`, `_divide_by_zero`, `_stack_overflow`,
  `_abort`, `_assert` and `_terminate` each run a probe program that crashes that way. They check
  the report for the reason, the frame, a detail, the log line written before the crash and a
  stack trace, and on Windows a non-empty minidump.
- **A real game**: `dream_hello_game_crash` runs the hello game with `simulate = "null_pointer"`
  and checks its report for the game's title, frame 2, its platform module and the log.
- An uncaught standard library exception was also tried. The report read "std::terminate was
  called: an exception nobody caught, of type St12out_of_range", and the standard library's own
  message still followed it.

All of this was compiled with GCC and run on Linux, also under AddressSanitizer and
ThreadSanitizer. On Android, the core and runtime tests and the probe's six crashes were run from
`/data/local/tmp` on a Samsung Galaxy A57 (arm64, Android 17). Each wrote a report with its
reason, frame, detail, log and stack, and `llvm-addr2line` named the probe's frames down to
`main`. The Windows handler (`crash_handler_windows.cpp`) could not be compiled there.
Running the `dream_crash_report_*` and `dream_hello_game_crash` tests on Windows confirms it, and
opening one of their `.dmp` files in Visual Studio confirms the minidump.

## 7. Limits

- **Some endings can't be caught** by anything inside the process: `__fastfail` on Windows, which
  the heap uses for some corruption; being killed, such as by Linux's out-of-memory killer or the
  Task Manager; a hang; power loss.
- **Linux and Android reports give offsets, not names**, for the game's own code. Naming them
  would need the program's symbols exported, which would break Vulkan loading (ADR 0019).
  `addr2line` names them (section 3).
- **A stack overflow on another thread** is reported on Windows, but not on Linux or Android,
  where only the main thread has a stack for the handler.
- **Reports stay on the machine.** Nothing is uploaded. A game that wants reports from players
  needs to send the files itself, or ask players for them.
- **A division by zero doesn't crash on ARM**, which returns 0 instead of trapping. The
  `divide_by_zero` simulation falls back to `abort()` there.
- **Android reports stay in the app's storage**, which only a debug build lets `adb` read.
