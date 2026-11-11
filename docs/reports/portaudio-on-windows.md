# Report: the portaudio audio module on Windows

## Summary

After `dream_arcade_demo` was run with the `portaudio` audio module on Windows, the console showed
these lines, and no sound played:

```
before paHostApiInitializers[0].
after paHostApiInitializers[0].
before paHostApiInitializers[1].
PA - PlatformId = 0x2
PA - MajorVersion = 0x6
PA - MinorVersion = 0x2
...
```

There were two separate problems:

1. **The lines themselves are not errors.** They are PortAudio's own debug trace. vcpkg builds the
   Debug version of PortAudio with its trace turned on, and PortAudio prints that trace straight to
   stderr. It looks like an error log, but it only describes PortAudio starting up.
2. **The real fault comes later in that output.** The module asked Windows for an audio format
   that WASAPI's shared mode won't take unless it is the device's exact mixing format. When it
   isn't, opening the output fails and the module gives up without trying another way to play sound.
   This was reproduced (section 4). Only the first lines of the output were available, so the
   failure on your machine isn't confirmed, but this is the cause that fits: the trace appears, and
   then the module isn't used.

Both are fixed in `modules/audio/portaudio/src/portaudio_audio.cpp`. The console is now quiet. Each
Windows audio system is tried in turn until one opens. WASAPI streams ask Windows to convert the
format.

## 1. The messages: PortAudio's debug trace

### Where each line comes from

Every line is printed by PortAudio itself (version 19.7.0, the version vcpkg installs). None of them
comes from the engine.

| Line | Printed by | What it means |
|---|---|---|
| `before paHostApiInitializers[N].` / `after ...` | `src/common/pa_front.c`, `InitializeHostApis()` | `Pa_Initialize()` is starting up each Windows audio system in turn |
| `[0]` | `PaWinMme_Initialize` | MME |
| `[1]` | `PaWinDs_Initialize` | DirectSound |
| `[2]`, `[3]` | `PaWasapi_Initialize`, `PaWinWdm_Initialize` | WASAPI, WDM-KS |
| `PA - PlatformId = 0x2` | `src/hostapi/dsound/pa_win_ds.c`, `PaWinDS_GetMinSystemLatencySeconds()` | `GetVersionEx()` reports Windows NT (`VER_PLATFORM_WIN32_NT` = 2) |
| `PA - MajorVersion = 0x6`, `MinorVersion = 0x2` | same | Version 6.2. Windows 8 and later report 6.2 to programs that don't declare support for newer versions in their manifest. DirectSound uses this to choose a default latency. It is printed once for each DirectSound device |

### Why they appear

- PortAudio prints through `PA_DEBUG(...)` (and `DBUG(...)` in the DirectSound code). These macros
  do nothing unless PortAudio is compiled with `PA_ENABLE_DEBUG_OUTPUT`. When it is, they call
  `PaUtil_DebugPrint()`, which writes to **stderr** unless a program has installed its own callback.
- The trace can only be printed by a PortAudio compiled with `PA_ENABLE_DEBUG_OUTPUT`. vcpkg's
  PortAudio port turns it on for its **Debug** build. So the trace appears with a Debug build of the
  game, which links vcpkg's Debug `portaudio.dll`. A Release build links the Release DLL and
  shouldn't print it.
- On Linux the module had only been tested without an audio device (`[audio.portaudio] device =
  false`), where PortAudio is never started. The trace had therefore never been seen.

The part of the trace quoted above ends before anything useful. When opening fails, the important
messages come further down the same output, such as `WASAPI ERROR ...
AUDCLNT_E_UNSUPPORTED_FORMAT`, followed by the engine's own message (this is from the reproduction):

```
[WARNING] Skipping module 'portaudio' for interface 'audio': Could not open the audio output: Invalid sample rate
```

## 2. The fault: WASAPI's shared mode and the stream format

### Root cause

The module opened one output: WASAPI's default device, as **2 channels of 32-bit float** at the
device's default rate, with no options set for WASAPI. In WASAPI's shared mode, PortAudio 19.7 asks
Windows whether the device takes that exact format (`IAudioClient::IsFormatSupported`, in
`GetClosestFormat()` in `pa_win_wasapi.c`). Windows approves only the format its audio engine mixes
the device in. That format is often not 2-channel float:

- 5.1 or 7.1 speaker setups, "virtual surround" gaming headsets, and HDMI monitors or receivers
  mix in 6 or 8 channels.
- Some drivers and virtual devices mix in 16- or 24-bit integers.

When the formats differ, PortAudio fails `Pa_OpenStream` with a misleading error:

- "Invalid number of channels" when Windows proposes a closest format with another channel count.
- "**Invalid sample rate**" when Windows proposes nothing (`paInvalidSampleRate` is PortAudio's
  catch-all return in `GetClosestFormat()`).

The module then failed as a whole, because it tried only that one device. The engine's module
registry skipped `portaudio` and used the next audio module in the list (or none), so the game had no
PortAudio sound. DirectSound and MME would have played, because they convert formats themselves, but
they were never tried.

### Why the earlier testing missed it

The module's tests mix without an audio device, so they never open a stream. On a machine whose
device mixes in stereo float, the original code works, which is why it wasn't noticed.

## 3. The fix

All changes are in `modules/audio/portaudio/src/portaudio_audio.cpp`. No game or configuration
changes are needed.

1. **WASAPI streams ask Windows to convert their format.** The module passes a
   `PaWasapiStreamInfo` with `paWinWasapiAutoConvert`. PortAudio then opens the stream with
   `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`, and Windows adds
   a channel matrixer and a sample-rate converter between the stream and the device's mixing format.
   Windows supports this from Windows 7 on. The engine's 2-channel float output then works whatever
   the device mixes in. The header this needs, `pa_win_wasapi.h`, is included only if it exists,
   because PortAudio installs it only when it is built with WASAPI, as vcpkg's is.

2. **If an output doesn't open, the next one is tried.** The module tries the default output of
   WASAPI, then DirectSound, then MME, then PortAudio's own default, and skips duplicates. It uses
   the first output that opens *and starts*. Each failure is logged as a warning that names the
   device and audio system. For example:

   ```
   [WARNING] PortAudio: could not open 'Speakers' through Windows WASAPI: Invalid device
   [INFO] PortAudio: playing on 'Primary Sound Driver' through Windows DirectSound at 44100 Hz
   ```

   If nothing opens, the error lists every attempt. If there is no output device at all, it says
   "There is no audio output device". Each attempt gets its own mixer at that device's rate, and a
   stream that opens but fails to start is closed before the next attempt.

3. **PortAudio's debug trace goes to the engine's log, at the debug level.** Before
   `Pa_Initialize()`, the module installs a callback with `PaUtil_SetDebugPrintFunction`. The trace
   arrives in fragments, so the callback joins them into whole lines (separately for each thread,
   because PortAudio's audio thread prints too) and logs each line as `[DEBUG] PortAudio: ...`. The
   engine logs at the info level by default, so the console stays quiet. To see the trace, call
   `dream::core::set_log_level(dream::core::LogLevel::Debug)`.
   - `PaUtil_SetDebugPrintFunction` isn't in PortAudio's installed headers, so the module declares
     it itself.
   - It is exported unconditionally on Windows: it is ordinal 55 in PortAudio's `.def` file, and
     static libraries contain it.
   - The Linux system package doesn't export it, so the call is made on Windows only.
   - In a Release build of PortAudio the callback is never called.

Also in this change: the PortAudio section of `docs/adr/0014-more-backends-and-libraries.md`
describes the output order, auto-conversion and where the debug trace goes.

## 4. How it was reproduced and checked

There was no Windows machine available, so the Windows build was reproduced as closely as possible:

- **PortAudio 19.7.0**, the same version vcpkg uses, from its release source. It was cross-compiled
  for Windows with MinGW-w64 as a Debug DLL, with `PA_ENABLE_DEBUG_OUTPUT=ON` and the MME,
  DirectSound, WASAPI and WDM-KS host APIs, as vcpkg builds it.
- The **module and its tests**, cross-compiled for Windows with the engine's own CMake scripts.
- A small **probe program** that uses the module as `dream_arcade_demo` does: it creates it through
  the module registry with a real device, loads `samples/assets/sounds/*.wav`, plays the music on a
  loop and the effects over it, and calls `update()` every frame.
- Both ran under **Wine 9**, whose MME, DirectSound and WASAPI implementations PortAudio uses as it
  would on Windows. Audio went to an ALSA virtual device that writes what it receives to a file, so
  it could be checked that sound really came out.

| Case | Before the fix | After the fix |
|---|---|---|
| Device mixing in 2-channel float | Plays, and the console shows the trace quoted above, line for line | Plays through WASAPI (with auto-conversion requested); console clean |
| Device whose mixing format isn't 2-channel float | `Could not open the audio output: Invalid sample rate`, and the module is skipped | WASAPI fails and is logged, then it plays through DirectSound. The output file has sound in it |
| No audio device | — | Module skipped with "There is no audio output device" |
| Module tests (Windows build, Wine) | 10/10 | 10/10 |
| All module and library tests (Linux: `-Werror`, plus ASan/UBSan) | 8/8 | 8/8 |
| Clang, Windows target, engine warning flags, `-Werror` | — | No warnings |

## 5. Limits of this verification

- **Wine doesn't implement `AUTOCONVERTPCM`.** Wine sends the stream format straight to the sound
  driver. The "after" result for a device with a different format therefore came from the
  DirectSound fallback, not from WASAPI's conversion. That Windows converts such streams is
  Microsoft's documented behaviour for the flag, and PortAudio documents `paWinWasapiAutoConvert`
  for this purpose. On real Windows, the expected result is `playing on '...' through Windows
  WASAPI` even for 5.1/7.1 devices.
- **Not yet run on real Windows.** It hasn't been built with MSVC against vcpkg's PortAudio or run
  on real hardware. To confirm:
  1. Run the Debug arcade demo with `audio = ["portaudio", "null"]`.
  2. Check that the console has no PortAudio trace.
  3. Check that there is a `PortAudio: playing on ... through Windows WASAPI` line.
  4. Check that sound plays.
