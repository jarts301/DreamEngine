# 0043. Crash uploads and texture cookers as choices

- Status: Accepted
- Date: 2026-11-20

## Context

The roadmap listed two pieces of modular debt: features that grew inside the engine before every
feature had to be a module (see its section 1).

- **Crash reports** were written to local files (ADR 0017), and that was all: a game's makers
  could see a player's crash only if the player sent the file. Uploading them was to become a
  `crash_upload` interface, with `sentry` and `backtrace` modules.
- **Texture cooking** was hard-wired to KTX2 with UASTC data (ADR 0024). Some games need smaller
  downloads, and pixel art, compression's blotches.

The engine has no HTTP library, and adding one, such as libcurl with a TLS library for Android, is
a large dependency for one feature.

## Decision

### Crash uploads: an interface, with the platform sending

`crash_upload::CrashUploader` turns one saved report, its text and its minidump where there is
one, into the requests its service takes, and sends them through a `Sender` it is given. A module
needs no network code of its own, and its tests check the requests without a network.

| Module | Sends |
|---|---|
| `null` | Nothing. Reports stay on the device |
| `sentry` | A report with a minidump to the project's minidump endpoint, the text attached; one without, as on Android, as a fatal event in an envelope, the text attached. `[crash_upload.sentry] dsn` and an optional `environment` |
| `backtrace` | A minidump to the `/minidump` endpoint, the text and attributes attached; one without as Backtrace's JSON crash, the text as an annotation. `[crash_upload.backtrace] url` |

Both follow their services' documented formats; they were checked against a local server, not
yet against real Sentry and Backtrace projects.

`runtime::CrashUploads`, which `run_program` starts once the platform is up, finds the reports
earlier runs saved, oldest first, up to `[crash_upload] max_reports` (5), and uploads them on a
thread of its own while the game runs. A report its service took moves, with its minidump, into a
`sent` folder beside it; at the first failure, usually the network's, the rest wait for the next
start. A module that can't be set up, such as `sentry` without a DSN, is passed over for the next
in the list, and the game runs.

The requests go through a new optional platform capability, `Platform::post_http`, which `sdl3`
gives with each system's own HTTP client: WinHTTP on Windows, and Java's `HttpURLConnection`
through JNI on Android. Linux has none yet, and its games keep their reports.

### Android permissions from modules

An Android app must ask for INTERNET to use the network, which a game that doesn't upload
shouldn't. `dream_add_module` takes `ANDROID_PERMISSIONS`, which `sentry` and `backtrace` give as
INTERNET, and a game's APK asks for the permissions of the modules it links and no others: the
Gradle build adds them to a copy of the manifest.

### Texture cookers: one file each, chosen per platform

A texture cooker is a CMake file in `cmake/cookers/textures/` that says how `ktx create` cooks an
image of each kind, color, normal or linear. A project picks one in its `dream.toml`, for every
platform or for one:

```toml
[cook]
textures = "ktx2_uastc"

[cook.android]
textures = "ktx2_etc1s"
```

| Cooker | Gives |
|---|---|
| `ktx2_uastc` | The default, as before: UASTC, high quality, about a quarter of a PNG's GPU memory |
| `ktx2_etc1s` | Basis Universal's ETC1S: much smaller files, at a visible cost in quality; normal maps stay UASTC |
| `none` | The PNGs as they are: lossless, but four times the GPU memory and slower to load |

A project can add its own in a folder it adds to `DREAM_TEXTURE_COOKER_DIRS`, and name it in its
`[cook]` table. The runtime already read both kinds of KTX2 data, so it didn't change.

## Consequences

- Crash reports reach a game's makers without players doing anything, on Windows and Android,
  with no new library: tested end to end with a local server on both.
- Reports describe the player's device: its modules, memory, and the log's last lines. A game must
  ask its players first where the law requires, before listing a module that sends. A setting a
  player can change in the game, rather than in the project's configuration, is a later step.
- Uploading starts with the game, and at exit waits for the request under way, whose every step
  times out after ten seconds without a network.
- Android games ask for exactly the permissions their modules need.
- Texture formats are a project's choice per platform, and new formats a cooker file, not an edit
  to the build.
- Crash uploads from Linux, and from consoles, need `post_http` there.
