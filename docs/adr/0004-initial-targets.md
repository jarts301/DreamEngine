# 0004. Initial targets

- Status: Accepted
- Date: 2026-11-09

## Decision

- **First game is 3D.** It tests the renderer and physics harder than a 2D game would.
- **SDL3 is the first platform backend.** It is one module behind the platform interface
  (`modules/platform/sdl3/`). Nothing outside that directory may include SDL headers, so a native
  backend can replace it through configuration.
- **Android minimum is API 29 (Android 10), arm64-v8a only.** This gives a reasonable Vulkan
  baseline.
- **Windows is x64, built with MSVC.**
- **The build uses CMake presets, Ninja and vcpkg manifest mode**, with static libraries and the
  dynamic CRT on Windows (`x64-windows-static-md`). Android uses NDK r30 with `c++_shared`, because
  a game will be a shared library alongside other shared libraries.

## Consequences

- Devices older than Android 10 and 32-bit devices are not supported.
- Android test binaries are compiled in CI but not run there until an emulator or device runner is
  added.
