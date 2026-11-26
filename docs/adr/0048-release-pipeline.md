# 0048. The release pipeline: versions, release packages with symbols, and release workflows

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 13 of the roadmap ends with a release pipeline: release builds made in CI, with their
symbols archived, versions taken from tags, and packages the stores take. Until now:

- The engine's version was written in `CMakeLists.txt` and nowhere else, and games had none: every
  APK said version 1.0, code 1, which Google Play refuses for a second upload.
- Release builds had no debug information. A crash report from a player's release build, with its
  minidump, showed addresses that nothing could turn back into functions.
- A PC game's package was a folder, and an Android game an APK; Google Play requires bundles.
- The engine's CI was paused, and built nothing to keep.

The engine and each game live in repositories of their own (ADR 0045): the engine in a public one,
each game in a private one, with its own secrets, such as signing keys and service tokens.

## Decision

Two pipelines, one per kind of repository, built on targets that work the same on a developer's
machine as in CI.

### Versions

- **The engine:** its version stays in `CMakeLists.txt`. An engine release is the tag
  `engine-v<version>`, and its workflow fails when the tag and the file disagree.
- **A game:** `dream_add_game` takes `VERSION`, or else the project's own, as in
  `project(my_game VERSION 1.4.2)`. A release sets `DREAM_GAME_VERSION` from its tag `v1.4.2`, which
  wins over both. A version is three whole numbers, with minor and patch below 100.

A game's version reaches:

| Where | As |
|---|---|
| The game | `runtime::game_version()`, and its log's first line: "Space Crates 1.4.2, on DreamEngine 0.2.0" |
| Its crash reports | A `Version:` line beside the engine's. An upload reads it from the report, so it names the version that crashed, which may be older than the one that uploads |
| Sentry and Backtrace | Sentry's `release`, as `<title>@<version>`, which its symbols are matched with; Backtrace's `version` attribute |
| Windows | The program's file properties, from a version resource the build makes |
| Android | `versionName`, and `versionCode` = major × 10000 + minor × 100 + patch, so 1.4.2 is 10402 |

### Symbols in release builds

Release builds now keep their debug information, apart from what ships:

- **Windows:** each object file carries it (`/Z7`), and the linker gathers it into the program's
  `.pdb` (`/DEBUG:FULL`, with `/OPT:REF` and `/OPT:ICF`, so the program stays the size it was).
- **Android:** `-g`. The library goes to Gradle with its symbols; Gradle strips them from the APK
  and bundle with the game's NDK, whose exact version the build now passes, and puts them in the
  bundle's metadata, where Google Play reads them for its crash reports and leaves them out of what
  players download. Debug builds keep them in the APK, as before.

### Release targets

| Target | Makes |
|---|---|
| `<game>_aab` | An Android App Bundle, as Google Play takes |
| `<game>_release` | In `<build>/<game>_release`: on PC, `<game>-<version>-windows.zip`, the package, and `<game>-<version>-windows-symbols.zip`, the program's `.pdb`; on Android, the APK, the bundle, and the unstripped library as `<game>-<version>-android-symbols.zip`, from one Gradle run |

For the render demo, a release is a 3 MB Windows zip with a 11 MB symbols archive, and a 4 MB APK
whose library is 6 MB stripped and 75 MB with symbols.

### The engine's release workflow

`.github/workflows/release.yml` runs on a tag `engine-v<version>`, on GitHub's machines, which a
public repository uses for free. It checks the tag's version, builds and tests the release build on
Windows with vcpkg's built libraries cached between runs, installs the SDK, and publishes
`DreamEngine-<version>-sdk-windows.zip` and its tools' symbols on a GitHub release. The test CI
stays started by hand until it is turned back on.

### Each game's release workflow

`dream new` writes `.github/workflows/release.yml` into each project. On a tag `v<version>`, it
builds `<game>_release` for Windows and Android at that version, uploads the symbols to Sentry when the
repository has a `SENTRY_AUTH_TOKEN` secret and `SENTRY_ORG` and `SENTRY_PROJECT` variables, and
publishes the files on the game's GitHub release, symbols included, as the record of every
version's.

It runs on a self-hosted runner, a machine of the game's makers, which costs no minutes and keeps
its build folders and vcpkg's libraries between runs, so a release rebuilds only what changed. The
runner's environment gives the SDK, vcpkg and Android's tools, as a developer's does. Moving to
GitHub's machines means changing `runs-on` and setting those up in the workflow.

### Installers

No installer for now: Steam installs from depots, GOG makes its own, and a zip suits itch.io and
direct downloads. The roadmap lists the options for later: NSIS, Inno Setup, WiX and MSIX.

## Consequences

- A version is set once, by a tag, and is the same in the game, its log, its crash reports, its
  files and its store listing.
- Crashes in release builds can be read: the symbols of every released version are kept with it,
  and Sentry and Google Play get theirs.
- Release builds take longer to compile and link, and their build folders are larger, with the
  debug information they now keep.
- The release workflows can't be run before the repositories and a runner exist, so they were
  checked by building the same targets with the same settings; their GitHub steps run first with
  the first real release.
- Still to do in Phase 14: signing with real keys kept outside the repositories, Play App Signing,
  store uploads, and Steam depots; and a Linux SDK, once the editor and tools are checked there.
