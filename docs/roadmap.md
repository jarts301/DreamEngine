# DreamEngine: where it stands, and where it goes next

This document has three parts:
- how a game is made with the engine today, and what kind of game that can be;
- what is missing before a game can be published professionally on Steam, Google Play, the App
  Store and similar stores;
- the plan to close those gaps, including macOS, iOS and online play.

It reflects the repository on 2026-11-18. The plan is a proposal: each phase gets its own decisions
and ADRs when it starts.

## 1. The rule every plan follows: everything is a module

DreamEngine is built so that any capability can be swapped by editing a project's `dream.toml`.
Every feature is an **interface** in `interfaces/`, with one or more **backends** in `modules/`,
always including a `null` backend. The build compiles only the backends a project lists, and the
first one that starts at run time is used ([ADR 0006](adr/0006-module-system.md)).

Only the mandatory core, which every game needs and no game would swap, is not modular:

| Not modular | What it is |
|---|---|
| Core (`core/`) | Logging, assertions, `Result`, math, memory, jobs, configuration, the module registry, crash handling, process statistics |
| Runtime spine (`runtime/`) | The `App` loop, the ECS world (EnTT), the asset manager, events, the virtual file system, scene loading, input actions |
| Build system (`cmake/`) | `dream_add_game`, module selection, shader compilation, asset staging and cooking |

Every new feature in this plan follows that rule: networking, store services, animation, the
editor, analytics and new platforms are each an interface with modules, and a `null` backend
that lets a game run without it.

### Modular debt in the code today

A few features grew inside the runtime before this rule was written down. They work, but should
move behind interfaces:

| Feature | Today | Should become |
|---|---|---|
| Crash reports | Built into Core, writing local files | Local capture stays in Core; uploading becomes a `crash_upload` interface: `none`, `sentry`, `backtrace` |
| Texture cooking | Hard-wired to KTX2 and UASTC in the build | A cooker step per format, chosen like shader formats |
| Game UI glue | `runtime/ui.h` over the `ui` interface | Fine as it is: the backends are already modules |

The 3D renderer was the largest of these. It is now the `renderer` interface, with the `forward`
and `null` modules ([ADR 0030](adr/0030-renderer-module.md)).

## 2. Making a game today

### The steps

1. **Create the game's folder inside the engine repository**, for example `samples/my_game/`, and add
   it to `samples/CMakeLists.txt`. There is no installable SDK or project template yet: games build
   as part of the engine tree.
2. **Declare the game in CMake:**

   ```cmake
   dream_add_game(my_game
       SOURCES src/my_game.cpp
       CONFIG dream.toml
       ASSETS assets ../assets
       APPLICATION_ID com.studio.mygame
       DISPLAY_NAME "My Game"
   )
   ```

   This builds a Windows executable, or on Android a native library and a `<name>_apk` target. It
   compiles the game's Slang shaders for every graphics backend it lists, cooks its textures, stages
   its assets, and on PC adds a `<name>_package` target that writes `assets.pak`.
3. **Write `dream.toml`.** It sets the window, the module lists per platform, input bindings, and
   each system's settings:

   ```toml
   [app]
   name = "MyGame"
   title = "My Game"

   [input.actions]
   jump = ["key:space", "touch"]

   [modules.windows]
   platform = ["sdl3"]
   rhi = ["vulkan", "directx12", "directx", "opengl", "null"]
   renderer = ["forward", "null"]
   mesh_simplifier = ["meshoptimizer", "null"]
   tangent_generator = ["mikktspace", "null"]
   physics3d = ["jolt"]
   audio = ["miniaudio", "null"]
   ui = ["rmlui"]

   [modules.android]
   platform = ["sdl3"]
   rhi = ["vulkan", "opengles", "null"]
   renderer = ["forward", "null"]
   mesh_simplifier = ["meshoptimizer", "null"]
   tangent_generator = ["mikktspace", "null"]
   physics3d = ["jolt"]
   audio = ["miniaudio", "null"]
   ui = ["rmlui"]

   [renderer]
   hdr = "auto"

   [renderer.shadows]
   mode = "auto"
   ```

   Other tables are `[memory]`, `[hot_reload]`, `[profiler]`, `[crash_report]` and `[screenshot]`.
4. **Write the game in C++20.** Subclass `runtime::Game`, return it from `runtime::create_game()`,
   and fill in the hooks you need: `on_start`, `on_update`, `on_fixed_update` (physics),
   `on_render`, `on_stop`, `on_pause`/`on_resume`, the surface events and `on_low_memory`. The `App`
   gives you:
   - the graphics device;
   - the ECS world, and scenes loaded from TOML files;
   - assets that hot-reload, and events;
   - input actions, and files under `assets:/` (read-only) and `user:/` (saves);
   - the renderer, 3D and 2D physics, audio, game UI, debug UI and the profiler.
5. **Build and run** with the presets:
   - `windows-debug` and `windows-release`;
   - `android-arm64-debug` and `android-arm64-release`, then install the APK with `adb`.
6. **Test** with screenshot checks, which compare a frame with a reference image, and with scripted
   runs on the `null` backends in CTest. Profile with the `timeline` profiler and Perfetto.

**Toppler** (`samples/toppler`) is the complete example: a 10-level physics puzzle with menus,
saved progress, music and settings, built this way and played on PC and phones.

### What a new project can make today

| Area | Available now |
|---|---|
| Platforms | Windows, and Android phones (arm64, Android 10 and up). Linux builds but is untested. |
| Graphics | Five graphics APIs; the same picture on each. Static glTF models with metallic-roughness PBR materials, normal, occlusion and emissive maps; one directional light with soft cascaded shadows; HDR with PBR Neutral tone mapping; frustum culling, instancing and levels of detail made as models load; debug lines. |
| Textures | PNG cooked to KTX2, sent to the GPU as BC7 on PC and ASTC on phones. |
| Physics | 3D (Jolt or the engine's own impulse solver): bodies, joints, triggers, ray casts. 2D (Box2D or Chipmunk). |
| Audio | WAV, MP3 and FLAC; streamed music; buses with volumes; sounds placed in 3D. |
| Interface | Menus and HUDs written in RML and RCSS (RmlUi or the engine's own `markup`); Dear ImGui for debug tools. |
| Input | Keyboard, mouse and touch, bound to named actions in the config. |
| Data | Scenes as TOML; saves and settings written to the user folder. |
| Workflow | Hot reload of shaders, models and textures; profiling; memory budgets; crash reports with stack traces and minidumps; screenshot tests. |
| Shipping | A PC folder with the executable and `assets.pak`; an Android APK signed with the debug key. |

Good fits today: puzzle and physics games, arcade and casual mobile games, small 3D scenes with
static props, prototypes and game jams.

Not yet possible without writing engine code first:
- **Characters:** animated characters (no skinning or animation).
- **Rendering:** environment lighting and transparency (materials are PBR, but reflect only the
  sun and a flat ambient, and draw opaque), more than one light, visual effects (no particles), very large worlds (culling, instancing and LODs exist, but no streaming
  or spatial structures), 2D sprite games (no sprite renderer).
- **Input:** gamepads.
- **Online:** any online play.

## 3. What is missing for a professional, published game

### 3.1 Engine features

| Need | Why it matters | As a module |
|---|---|---|
| Gamepad input | Required on Steam Deck and expected on PC and TV; common on phones | `input_device`: `sdl3_gamepad`, `null` |
| Skeletal animation | Characters, creatures, anything that moves by itself | `animation`: `ozz` (MIT), `null` |
| PBR materials and normal maps | glTF assets look as their artists made them | Done in `forward`, with tangents from `tangent_generator`: `mikktspace`, `null` (ADR 0033) |
| Point and spot lights, many lights | Interiors, night scenes, effects | `renderer`: `forward_plus` |
| Particles | Fire, smoke, impacts, magic | `particles`: GPU or CPU backend, `null` |
| Culling, instancing, LOD | Scenes with thousands of objects | Done in `forward` and `mesh_simplifier` (ADR 0031, 0032); a `visibility` interface later for spatial structures |
| 2D sprites and tile maps | 2D games, which the 2D physics already supports | `sprite_renderer`: `batch`, `null` |
| Localization | Stores expect several languages; Play and the App Store list them | `localization`: `fluent` or `gettext`-style files, `null` |
| Accessibility | Platform reviews and players ask for it: text size, colour-blind modes, subtitles, remapping | Settings over existing modules |
| Settings menu and display modes | Resolution, fullscreen and borderless, frame cap, quality presets | A `settings` service in the runtime spine, with values from each module |
| Save system | Versioned saves, migration, corruption safety, cloud sync hook | `save_storage`: `local_files`, `steam_cloud`, `play_saved_games`, `icloud` |
| Asset streaming | Big games can't load everything at start | `asset_source`: `loose_files`, `pak`, `bundles` |
| Scripting (optional) | Designers iterate without recompiling | `scripting`: `lua` (MIT) or `wasm`, `null` |

### 3.2 Tools

| Need | Today | Missing |
|---|---|---|
| Project setup | Games live in the engine tree | An installable SDK: `find_package(DreamEngine)`, a `dream new` command and a template |
| Editor | None | Scene editing, an entity inspector, an asset browser and play-in-editor. It would be a tool program built on the engine, with its panels as modules on `debug_ui` |
| Asset pipeline | glTF, PNG to KTX2, Slang | Importers for FBX and audio, an asset database with dependencies, per-platform bundles |
| Release pipeline | CI builds and tests on push (paused for now) | Release builds with symbols kept and uploaded, versioning, signed artifacts, store uploads |

### 3.3 Store requirements

**Steam (Windows, and later Linux and macOS)**
- Steamworks integration: achievements, cloud saves, the overlay, rich presence, Steam Input and
  DRM if wanted. **The Steamworks SDK is not permissively licensed**, so under
  [ADR 0022](adr/0022-licenses-and-frozen-libraries.md) it can't be a dependency in this
  repository. It has to be a **private, out-of-tree module** (`store_services: steam`), as the plan
  already intends for consoles.
- Steam Deck Verified expects:
  - full gamepad support;
  - legible text at 1280×800;
  - an on-screen keyboard for text input;
  - running on Linux natively or under Proton.

  That makes Linux testing part of the Steam work.
- Display options (fullscreen, borderless, resolution, V-sync) and a settings menu.
- Release builds with symbols kept for crash analysis.

**Google Play**
- **Android App Bundle (AAB)** instead of APK, and a real release key with Play App Signing. The
  template signs release builds with the debug key today.
- Raise `targetSdk` each year to the level Play requires. The template targets 35.
- Version codes from the build, 64-bit only (already the case), and 16 KB memory page support for
  newer devices (check the NDK and libraries).
- Play Billing for in-app purchases, and Play Games Services for achievements and saved games.
  These are proprietary SDKs, so they are out-of-tree modules too.
- The Data safety form and a privacy policy. A consent dialog (UMP) if there are ads or analytics.
- Adaptive icons, store screenshots, and the release-build crash and vitals data Play shows.

**App Store (iOS and iPadOS) and Mac App Store**
- Code signing, provisioning profiles, App Store Connect, TestFlight.
- StoreKit for purchases, Game Center for achievements and leaderboards, iCloud for saves. These
  are system frameworks, so the restriction is Apple's terms, not the license policy.
- A privacy manifest, app icons and launch screens, ProMotion frame pacing, and safe areas (the
  engine already reports these).
- On the Mac outside the store: notarization and universal binaries (arm64 and x86-64).

**Every store**
- Age ratings (IARC), store pages, a crash-report backend, analytics if wanted, terms and privacy
  documents, and a support contact.

## 4. The plan

The original roadmap ended with Phase 12 (feature backlog) and Phase 13 (new platforms). This plan
keeps Phase 12, and splits what follows into tools, store readiness, Apple platforms, online play,
and the remaining platforms.

### Phase 12: features (in progress)

- **Done:** render targets (ADR 0027), HDR and tone mapping (ADR 0028), cascaded shadows
  (ADR 0029), the OpenGL backends (ADR 0026), the renderer as a module (ADR 0030), frustum
  culling (ADR 0031), instancing and levels of detail (ADR 0032), and PBR materials with normal
  maps (ADR 0033, with a [guide](guides/materials.md)).
- **Next, in order:**
  1. Point and spot lights, with `forward_plus` for many lights.
  2. Skeletal animation (`animation: ozz`).
  3. Particles (`particles`).
  4. Gamepads (`input_device: sdl3_gamepad`).
  5. 2D sprites (`sprite_renderer`).
  6. Materials, after ADR 0033: environment lighting (image-based, with reflection probes),
     alpha-tested and blended materials, double-sided materials, reading `.gltf` files so
     Blender's images are cooked, and half-precision lighting math for phones (PBR costs 1.4 ms
     of GPU time per frame on the A52s at full screen; see the Android performance report).
  7. Smaller items: blending between shadow cascades, a frame-rate cap, removing the ~13
     allocations per frame found on the A52s, and cooking levels of detail at build time instead of
     when models load.
  8. A phone check per GPU family: run the render samples' pictures on Adreno, Mali and Xclipse
     phones before a rendering feature is called done. The cascaded shadows went unseen on Adreno
     for a day, because only the A57's Xclipse GPU had been checked (ADR 0029). The PBR materials
     (ADR 0033) were checked on the A52s's Adreno, with Vulkan and OpenGL ES; the A57's Xclipse and
     a Mali phone are still to do.

### Phase 13: tools for making games

- **Installable SDK:** CMake package config, `dream new <name>` with a project template, and games
  in their own repositories.
- **Editor v1:** a program built on the engine. It needs:
  - a scene view;
  - an entity tree and component inspector, with components describing their fields through a
    reflection registry;
  - saving scenes to TOML;
  - play-in-editor using the existing hot reload.

  Panels are `editor_panel` modules over `debug_ui`, so a project can add its own.
- **Asset database:** dependencies, cooking per platform, bundles (`asset_source: bundles`).
- **Release pipeline:**
  - CI release builds, with symbols archived;
  - versioning from tags;
  - a `<name>_aab` target and a `<name>_installer` target where a store needs one.

### Phase 14: store readiness, Windows and Android

- **Store services interface:** `store_services`, covering achievements, leaderboards, stats, cloud
  saves, purchases and presence. Backends:
  - `null`;
  - `local`, which saves achievements to disk for testing;
  - out-of-tree `steam` and `google_play`.
- **Consent and analytics:** a `consent` interface (`null`, `ump`) and `analytics` (`null`,
  `local_log`, an open backend such as PostHog through HTTP).
- **Crash uploads:** `crash_upload` with `sentry`.
- **Player-facing basics:** a settings menu template, display modes, localization, accessibility
  options, input remapping.
- **Store build artifacts:** AAB, release signing from a keystore outside the repo, target SDK
  updates, Play feature checks, Steam depots.
- **Linux:** made a tested platform, for Steam Deck.
- **Pass condition:** Toppler, updated, passes Steam Deck Verified checks and the Google Play
  pre-launch report.

### Phase 15: Apple platforms, macOS and iOS

- **Platform:** `sdl3` already supports both.
- **Graphics:** a native `metal` RHI module, with shaders compiled by Slang to Metal. Vulkan on
  MoltenVK (Apache 2.0) can come first as a quick start, since Slang and the `vulkan` module
  already work.
- **Audio:** `miniaudio` already speaks CoreAudio.
- **Build:**
  - CMake presets with the Xcode generator;
  - `macos-universal` and `ios-arm64` toolchains, built in CI on Apple runners;
  - signing, provisioning and notarization from settings outside the repository.
- **Packaging:** a `.app` bundle on macOS and an `.ipa` on iOS, from `dream_add_game`, as the APK
  is today. The touch, lifecycle and surface-loss work done for Android carries over.
- **Store services:** `store_services: game_center`, StoreKit purchases, `save_storage: icloud`.
- **Pass condition:** Toppler and the render demo match their screenshot references on a Mac and an
  iPhone, and Toppler goes through TestFlight.

### Phase 16: online play

Online play splits into four interfaces, so a game takes only what it needs and each part can be
replaced.

| Interface | Job | Backends |
|---|---|---|
| `net_transport` | Moving packets: reliable and unreliable channels, connections | `enet` (MIT), `gns` (Valve's GameNetworkingSockets, BSD-3), `websocket` for browsers and restrictive networks, `loopback` for tests, `null`; out of tree: `steam_networking` |
| `net_replication` | Keeping the world in sync | `snapshot` (server-authoritative state with interpolation and client prediction), `rollback` (deterministic inputs, for fighting and action games), `null` |
| `online_services` | Accounts, friends, presence, lobbies, matchmaking, leaderboards | `nakama` (Apache 2.0, self-hostable), `local` for tests, `null`; out of tree: `steam`, `epic`, `google_play`, `game_center` |
| `voice_chat` (later) | Voice between players | `opus` (BSD), `null` |

**Steps:**
1. `net_transport` with `enet` and `loopback`. A two-player version of the arcade sample, over a LAN.
2. `net_replication: snapshot`: entities and components marked for replication, prediction and
   reconciliation, and lag and loss simulation in the transport for testing.
3. **Dedicated servers:** a headless build is already possible with the `null` platform, graphics
   and audio modules. Add a server preset, a Linux server container, and documentation for
   hosting.
4. `online_services: nakama`: sign-in, lobbies and matchmaking, and a relay for players behind
   NAT.
5. `net_replication: rollback` for games that need it, and `steam_networking` out of tree.
6. **Security:**
   - encrypted transport, with libsodium (ISC) where the transport has none;
   - server authority, so the client is never trusted;
   - rate limits and basic cheat detection.

**Pass condition:** two phones and a PC play the same match through a dedicated server, at 150 ms
of simulated latency and 5% packet loss, and the match stays in sync.

### Phase 17: more platforms

- **Consoles** (Xbox, PlayStation, Switch): private, out-of-tree modules under each vendor's
  developer agreement: platform, graphics API, store services. The interfaces built earlier are
  the points they plug into.
- **Web:** Emscripten, with `rhi: webgpu` and `net_transport: websocket`, as an option for demos.
- **Android TV, Chromebooks and foldables,** once gamepads and resizable windows are in.

## 5. Decisions to make before these phases

1. **Proprietary store SDKs:** keep them only as private, out-of-tree modules (the console
   approach), or allow a written exception in ADR 0022 for platform-vendor SDKs in an optional
   folder that is never built by default.
2. **Editor technology:** an engine-hosted editor on Dear ImGui (fast to build, consistent with the
   debug UI), or a separate application with a native UI.
3. **Scripting:** none (C++ only), Lua, or WebAssembly. Each would be a module, but the choice
   affects the editor and hot reload.
4. **The renderer as a module:** decided and done ([ADR 0030](adr/0030-renderer-module.md)).
5. **Order after Phase 12:** this plan puts tools before stores and stores before Apple and online.
   Online play could move ahead of Apple if the first commercial game needs it.

## 6. Summary

The engine can already ship small, polished 3D games on Windows and Android phones, built from
modules that swap through configuration, and with the testing, profiling and crash tooling
professional work needs. Before a commercial release on Steam or Google Play, it most needs:
- gamepads, animation, richer materials and more lights;
- a settings menu, localization and store services;
- release packaging: AAB and real signing on Android, depots on Steam;
- an installable SDK and an editor.

Each of those is planned as a module, as is everything for macOS, iOS and online play, so a
project only carries what it uses.
