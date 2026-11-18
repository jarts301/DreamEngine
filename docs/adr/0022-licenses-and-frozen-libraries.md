# 0022. Wider third-party licenses, frozen in-house libraries, and platform focus

- Status: Accepted
- Date: 2026-11-17

## Context

ADR 0014 allowed third-party code only under the MIT or Apache 2.0 license. That rule turned
down Tracy (BSD 3-Clause), Bullet and SoLoud (zlib), and led to four in-house libraries:
`impulse`, `markup`, `mixer` and `timeline`. But the engine already used code outside it: SDL3,
the platform module everything runs on, is zlib, and RmlUi brings in FreeType, libpng, zlib and
bzip2, under licenses of the same kind. The rule was stricter on paper than in the build.

The in-house libraries work, and their tests pass on Windows and on the phone. They were written
to give every interface a second real backend (ADR 0001, rule 3), and they have done that.

The engine has Linux code paths, such as the crash handler, but no Linux build in CI, and the
plan's targets are PC and Android.

## Decision

### Licenses

Third-party code must be under one of these licenses, or one that grants as much and asks no
more:

| License | Examples in the engine |
|---|---|
| MIT, MIT No Attribution | Jolt, Box2D, EnTT, RmlUi, Dear ImGui, toml++, volk, Chipmunk2D, PortAudio |
| Apache 2.0, with or without the LLVM exception | Slang, the Vulkan validation layers |
| BSD 2-Clause and 3-Clause, and BSD-style ones such as FreeType's | FreeType, bzip2 |
| zlib, and zlib-style ones such as libpng's | SDL3, zlib, libpng |
| Public domain, or a public-domain dedication such as the Unlicense or CC0 | stb, dr_libs, miniaudio |

Copyleft licenses, such as the GPL, LGPL and MPL, stay out. A library offered under several
licenses is used under one from the table. Platform SDKs, such as the Windows SDK and the Android
NDK, are part of the platform, not dependencies, and aren't covered. Assets keep their own
licenses, such as the sample fonts' SIL Open Font License.

A new dependency names its license in the ADR or commit that adds it.

### The in-house libraries are frozen

`impulse`, `markup`, `mixer` and `timeline` stay, and stay built and tested on every platform.
Their backends remain selectable like any other. They are frozen:

- They get bug fixes, and the changes needed to keep building and passing when the engine, its
  interfaces or the compilers change.
- They get no new features. A feature an interface gains is required of their backends only when
  it is small. Otherwise their backend reports it as unsupported, and the first backend (Jolt,
  RmlUi, miniaudio) is where it is built.
- A frozen library that a third-party library can now replace, under the wider licenses, may be
  retired by a later ADR, with its backend moved onto that library.

### Platform focus

Work targets Windows and Android. Linux code paths stay as they are, and are kept compiling where
Android shares them, but Linux isn't built or tested in CI and isn't a supported platform for now.

## Consequences

- SDL3, FreeType and the libraries RmlUi brings are now within the rule, not exceptions to it.
- Tracy, Bullet and other zlib or BSD libraries can be considered again. None is added by this
  ADR.
- The asset cooking work can choose texture encoders from the wider set.
- The second backends of physics, audio and the game interface fall behind the first ones over
  time. Projects that need a newer feature list the first backend.
- Code shared by Linux and Android, such as the crash handler, is checked only through Android.
