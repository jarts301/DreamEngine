# 0039. Offline Linux build, used from a web browser

- Status: Accepted, on the `offline-web` branch
- Date: 2026-11-20

## Context

DreamEngine has to run on a machine that has:

- no internet, so nothing can be downloaded at build time or run time;
- no GPU, no display, no sound card, no Docker and no cloud services;
- Linux, and no Windows-only software;
- no apt-get, Go modules, NuGet, Ruby gems or native desktop toolkits, and a Gradle wrapper that
  may fail to download.

Everything it needs must be in the repository, and its interfaces and tests must work in a web
browser.

The main branch depends on vcpkg for 25 libraries, compiles its shaders with Slang, draws only with
GPU APIs (Vulkan, Direct3D 11 and 12, OpenGL), opens its windows with SDL3, plays sound with
miniaudio or PortAudio, and builds Android APKs with Gradle. None of that works there.

## Decision

A branch, `offline-web`, that keeps the engine's architecture and interfaces and changes only the
build and the set of backends, as the module system (ADR 0006) intends.

**Everything is in the repository.** `third_party/` holds the header-only libraries the engine
uses (doctest, EnTT, toml++, cgltf, stb) and the sources of the CPU-only compiled ones it keeps
(Box2D, ozz-animation, meshoptimizer, MikkTSpace, Dear ImGui), each with its license, built by
CMake with the engine. vcpkg, its manifest, ports and triplets, the presets that need it, and the
CI workflow are removed. The build needs only a C/C++ compiler and CMake 3.22.

**Removed**, because they need a GPU, a display, a sound card, Windows or Android, or libraries this
build doesn't carry: the `vulkan`, `directx`, `directx12`, `opengl` and `opengles` rhi modules; the
`forward` and `forward_plus` renderers and their Slang shaders; `sdl3` platform and gamepads;
`miniaudio` and `portaudio`; `rmlui`; `jolt` and `chipmunk`; the Android template and APK
packaging; texture cooking and the KTX library (games ship PNGs, and `load_ktx2` says so); the
Windows crash handler; and the `cube`, `render_targets` and `scene_demo` samples, which draw with
their own shaders.

**Added**, as modules and one library:

- `rhi/software`: a device that keeps buffers and textures in memory (making RGBA8 mipmaps
  itself) and owns the window's pixels. It can't run shaders, so it checks draw commands against
  the interface's rules, as `null` does, and offers a new optional capability,
  `Device::software_access()` (`rhi/software.h`), through which a renderer reads resources and
  writes the frame. It presents to a `platform::WebCanvas` and captures frames for screenshot
  tests.
- `renderer/software`: draws a `renderer::Frame` on the CPU with the forward renderer's lighting
  model (GGX, Smith visibility, Schlick Fresnel, ambient specular fit), a visibility buffer for
  opaque surfaces, sorted blending, a sun shadow map, point and spot lights, skinning, particles,
  debug lines and interface layers, then PBR Neutral tone mapping. The screen is cut into one band
  per thread.
- `platform/web`: the window is a web page the game serves itself. The page fetches each new frame
  as raw RGBA pixels (long polling) and posts keyboard, mouse, wheel, touch and text input back as
  lines of text. `NativeWindow` gains `WebWindow`, pointing at the platform's `WebCanvas`, and loses
  `Win32Window` and `AndroidWindow`. With `app.window = "hidden"` nothing is served, for automated
  runs. Files come from the program's folder and `~/.local/share`, or `DREAM_USER_DIR`.
- `libraries/webserve`: a small HTTP/1.1 server on POSIX sockets, a thread per connection. ADR 0022
  froze the in-house libraries; this one is new to this branch and follows the same rules (no
  engine code, only the standard library).
- `tools/hub`: one page from which to build, start and play the games, run the tests (`ctest`'s
  JUnit output, read back into a table) and compare screenshots.

The screenshot tests that ran on WARP now run on the software renderer, with references made by
it. Audio and gamepads have only their null backends, so games run silently and with mouse and
keyboard.

## Consequences

- The whole engine builds and its tests pass offline on a stock Linux with a compiler and CMake.
- Pictures are drawn on the CPU, so resolution and frame rate are lower: the samples draw 960x540
  at up to 30 frames a second, which is smooth on a few cores. Shadows come from one map, not
  cascades, and without percentage-closer filtering beyond 2x2.
- Frames travel uncompressed (2 MB each at 960x540), which suits a browser on the same machine or
  a local network, not the internet.
- The web page has no authentication. Games and the hub listen on 127.0.0.1 unless told otherwise.
- Programs that draw with their own shaders through the RHI show only their clear color on the
  software device. A game should draw through the scene renderer.
- The branch diverges from main in build files, sample configurations and the removed modules. The
  new modules and `software_access()` could be merged into main as they are, as more backends.
