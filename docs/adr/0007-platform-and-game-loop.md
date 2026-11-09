# 0007. Platform layer and game loop

- Status: Accepted
- Date: 2026-11-09

## Decision

### Platform interface

`interfaces/platform` covers a window, events and file storage. Events are a plain `std::variant` of
small structs, and input uses physical key positions. There are two backends: `sdl3` for PC and
Android, and `null` for automated runs.

Android is served by the SDL3 backend. A native Android backend can replace it through
configuration.

### Lifecycle

`AppPaused`/`AppResumed` and `SurfaceLost`/`SurfaceRestored` are separate events, because Android
doesn't deliver them in a guaranteed order. `App` updates the game only when it is both running and
has a surface. It ignores events that change nothing, and sleeps in `wait_event` otherwise.

SDL freezes programs inside its event functions by default while on Android's background, so the
game would learn it had been paused only after returning. The backend turns that off
(`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`) and `App` does the waiting.

### Game interface and entry point

A game derives from `runtime::Game` and defines `runtime::create_game`. `dream_add_game` compiles
`runtime/entry/game_entry.cpp` into each game, giving it `main` on PC and `dream_android_main` on
Android. Backends never own the entry point, and games never see one.

Time comes from `core::Clock`. Its cap on the frame delta is what absorbs a return from the
background. `app.max_fps` is a temporary limit until the renderer provides vertical sync.

### Virtual files

Games use `assets:/` for read-only shipped files and `user:/` for writable storage. The path rules
live in `runtime::VirtualFileSystem`, so every backend gets the same safety checks. Writes replace
files atomically in the SDL backend.

### Input actions

Games ask about named actions, and bindings live in the project configuration. Keys, mouse buttons
and touch are supported. Gamepads are not yet.

### APK packaging

CMake builds `libmain.so`, then a `<game>_apk` target runs one shared Gradle project,
`templates/android`, and passes it the game's details. Gradle compiles only Java. This keeps a
single native build path through the CMake presets, with no second vcpkg and CMake setup inside
Gradle.

A backend can ship Java in `android/java/`, and only selected backends are compiled into the APK.
SDL's Java files are copied into `modules/platform/sdl3/android/` because the vcpkg port doesn't
install them. SDL is linked statically into `libmain.so`, following ADR 0001.

## Consequences

- SDL's Java files must be copied again whenever the SDL version changes, or SDL refuses to start.
- Release APKs are signed with the debug key so they can be installed. Publishing needs a real key.
- Native libraries are unpacked at install time, which SDL's default activity requires. It costs some
  device storage.
- Only `arm64-v8a` is packaged, so x86-64 emulators can't run the APK. It needs a real device.
