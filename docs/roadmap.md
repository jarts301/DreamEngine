# DreamEngine: where it stands, and where it goes next

This document has three parts:
- how a game is made with the engine today, and what kind of game that can be;
- what is missing before a game can be published professionally on Steam, Google Play, the App
  Store and similar stores;
- the plan to close those gaps, including macOS, iOS and online play.

It reflects the repository on 2026-11-20. The plan is a proposal: each phase gets its own decisions
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

### Modular debt, now paid off

A few features grew inside the runtime before this rule was written down. All have since moved
behind interfaces or choices:

| Feature | Was | Now |
|---|---|---|
| Crash reports | Built into Core, writing local files only | Local capture stays in Core; uploading is the `crash_upload` interface: `null`, `sentry`, `backtrace`, sending through the platform (ADR 0043) |
| Texture cooking | Hard-wired to KTX2 and UASTC in the build | A texture cooker per format, chosen in `[cook]` per platform: `ktx2_uastc`, `ktx2_etc1s`, `none`, or a project's own (ADR 0043) |
| Reading cooked textures | libktx linked into every game's runtime | The `texture_decoder` interface: `ktx`, `null`; the build checks a game lists the decoder its cooker needs (ADR 0044) |
| Game UI glue | `runtime/ui.h` over the `ui` interface | Fine as it is: the backends are already modules |

The 3D renderer was the largest of these. It is now the `renderer` interface, with the `forward`
and `null` modules ([ADR 0030](adr/0030-renderer-module.md)).

## 2. Making a game today

### The steps

1. **Make the project** with the SDK's `dream` tool: `dream new my_game` makes `my_game/`, a
   repository of its own that builds the engine through `find_package(DreamEngine)`, from the SDK
   that `DREAM_ENGINE_SDK` names (ADR 0045). Install the SDK with
   `cmake --install <build> --prefix <folder>`, or use a checkout of the engine. The template
   already has steps 2 and 3 done, for a small 3D scene; the engine's own samples are made the same
   way, inside its repository.
2. **Declare the game in CMake:**

   ```cmake
   dream_add_game(my_game
       SOURCES src/my_game.cpp
       CONFIG dream.toml
       ASSETS assets
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
   texture_decoder = ["ktx", "null"]    # Reads the textures the build cooks.
   physics3d = ["jolt"]
   audio = ["miniaudio", "null"]
   ui = ["rmlui"]

   [modules.android]
   platform = ["sdl3"]
   rhi = ["vulkan", "opengles", "null"]
   renderer = ["forward", "null"]
   mesh_simplifier = ["meshoptimizer", "null"]
   tangent_generator = ["mikktspace", "null"]
   texture_decoder = ["ktx", "null"]    # Reads the textures the build cooks.
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
   - the ECS world, and scenes loaded from TOML files, which the editor edits (ADR 0046);
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
| Graphics | Five graphics APIs; the same picture on each. glTF models, static or skinned and animated, with metallic-roughness PBR materials, normal, occlusion and emissive maps, and opaque, cut-out, blended and double-sided surfaces; one directional light with soft cascaded shadows, and point and spot lights, up to 256 with `forward_plus`; lighting from an HDR environment, which shiny surfaces reflect; HDR with PBR Neutral tone mapping and bloom; frustum culling, instancing and levels of detail, made at build time; particles; 2D sprites, sprite-sheet animation and tile maps, with crisp pixel art; debug lines. |
| Textures | PNG cooked at build time by a cooker chosen per platform: KTX2 with UASTC, or the smaller ETC1S, sent to the GPU as BC7 on PC and ASTC on phones; or PNG kept as it is, as for pixel art. |
| Physics | 3D (Jolt or the engine's own impulse solver): bodies, joints, triggers, ray casts. 2D (Box2D or Chipmunk). |
| Audio | WAV, MP3 and FLAC; streamed music; buses with volumes; sounds placed in 3D. |
| Interface | Menus and HUDs written in RML and RCSS (RmlUi or the engine's own `markup`); Dear ImGui for debug tools. |
| Input | Keyboard, mouse, touch and gamepads, bound to named actions in the config, with analog values for sticks and triggers. |
| Data | Scenes as TOML; saves and settings written to the user folder. |
| Workflow | Hot reload of shaders, models and textures; profiling; memory budgets; crash reports with stack traces and minidumps, uploaded to Sentry or Backtrace from Windows and Android; screenshot tests. |
| Power | A frame limit, and a lower one on battery, which also asks a phone's display to slow to match. |
| Shipping | A PC folder with the executable and `assets.pak`; an Android APK signed with the debug key. |

Good fits today: puzzle and physics games, arcade and casual mobile games, 2D platformers and
pixel-art games, small 3D scenes with
static props, prototypes and game jams.

Not yet possible without writing engine code first:
- **Characters:** root motion, animation events, inverse kinematics, blend trees, facial morph
  targets and ragdolls (skeletal animation with fades between clips exists).
- **Rendering:** reflection probes for interiors (one environment lights the whole scene),
  shadows from point and spot lights, visual effects beyond particles, very
  large worlds (culling, instancing and LODs exist, but no streaming or spatial structures).
- **2D:** levels from Tiled, parallax backgrounds, animated tiles and lit sprites (sprites, sprite
  sheets, tile maps and tile collisions exist).
- **Input:** menus driven by a gamepad (no focus navigation in `ui`), and rebinding while the game runs.
- **Online:** any online play.

## 3. What is missing for a professional, published game

### 3.1 Engine features

| Need | Why it matters | As a module |
|---|---|---|
| Spatial structures and streaming | Very large worlds | A `visibility` interface, with `asset_source` streaming below |
| Localization | Stores expect several languages; Play and the App Store list them | `localization`: `fluent` or `gettext`-style files, `null` |
| Accessibility | Platform reviews and players ask for it: text size, colour-blind modes, subtitles, remapping | Settings over existing modules |
| Settings menu and display modes | Resolution, fullscreen and borderless, frame cap, quality presets | A `settings` service in the runtime spine, with values from each module |
| Save system | Versioned saves, migration, corruption safety, cloud sync hook | `save_storage`: `local_files`, `steam_cloud`, `play_saved_games`, `icloud` |
| Asset streaming | Big games can't load everything at start | `asset_source`: `bundles` and `files` exist (ADR 0047); still missing: loading a bundle on a thread of its own, and bundles downloaded on demand |

Already done in Phase 12, and no longer missing: PBR materials with normal, occlusion and emissive
maps, tangents from `tangent_generator` (ADR 0033); transparent, cut-out and double-sided materials
(ADR 0034); frustum culling (ADR 0031); instancing and levels of detail from `mesh_simplifier`
(ADR 0032); gamepads through `input_device` (ADR 0035); skeletal animation through `animation`:
`ozz`, `null`, with GPU skinning (ADR 0036); point and spot lights in `forward` and
`forward_plus` (ADR 0037); particles through `particles`: `cpu`, `null` (ADR 0038); 2D sprites,
sprite-sheet animation and tile maps with collisions, drawn by `forward` (ADR 0039), with parallax,
9-slice sprites, sorting by height and animated tiles (ADR 0049), and maps from Tiled through
`tile_map_format`: `tiled`, `null` (ADR 0050); environment
lighting from HDR images and bloom (ADR 0040); blending between shadow cascades, a frame limit on
battery and almost no heap allocations per frame (ADR 0041); levels of detail made at build time
(ADR 0042).

### 3.2 Tools

| Need | Today | Missing |
|---|---|---|
| Project setup | An installable SDK of the engine's sources, `find_package(DreamEngine)`, `dream new` with a template, games in their own repositories (ADR 0045) | A binary SDK if first builds get too slow; `dream` commands to add modules and update a game's `vcpkg.json` |
| Editor | Editor v1 (ADR 0046): a scene view, an entity tree, an inspector built from components' field descriptions, saving to TOML with undo, and Play through hot reload, with its panels as `editor_panel` modules | An asset browser, handles to move entities in the view, editing 2D scenes, multi-selection and copying, field descriptions for the remaining components |
| Asset pipeline | glTF with levels of detail made at build time; PNG to KTX2 by a texture cooker chosen per platform; Slang; an asset database of what each asset needs, which fails the build for a missing one, and bundles shipped apart (ADR 0047) | Importers for FBX and audio, Play Asset Delivery packs on Android, an asset browser in the editor |
| Release pipeline | Versions from tags; release builds with symbols kept apart and sent to Sentry and Google Play; `<game>_release` and `<game>_aab`; release workflows for the engine's SDK and, through `dream new`, for each game (ADR 0048). Test CI is paused | Signed artifacts, store uploads, a Linux SDK, and optional installers (below) |

### 3.3 Store requirements

**Steam (Windows, and later Linux and macOS)**
- Steamworks integration: achievements, cloud saves, the overlay, rich presence, Steam Input and
  DRM if wanted. **The Steamworks SDK is not permissively licensed**, so under
  [ADR 0022](adr/0022-licenses-and-frozen-libraries.md) it can't be a dependency in this
  repository. It has to be a **private, out-of-tree module** (`store_services: steam`), as the plan
  already intends for consoles.
- Steam Deck Verified expects:
  - full gamepad support: gamepads drive actions (ADR 0035), but menus need gamepad focus
    navigation;
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
- Age ratings (IARC), store pages, a crash-report backend (uploads to Sentry or Backtrace exist,
  ADR 0043), analytics if wanted, terms and privacy documents, and a support contact.

## 4. The plan

The original roadmap ended with Phase 12 (feature backlog) and Phase 13 (new platforms). This plan
keeps Phase 12, and splits what follows into tools, store readiness, online play, Apple platforms,
and the remaining platforms.

### Phase 12: features (in progress)

Phase 12 adds the features games most often need, each as a module or a renderer setting, checked
on the five PC graphics backends and on a phone.

**Done**

| Area | What works now | Design | Guide |
|---|---|---|---|
| Rendering base | The OpenGL backends; render targets; HDR with PBR Neutral tone mapping; the renderer as a module | ADR 0026, 0027, 0028, 0030 | |
| Shadows | Cascaded shadows from the sun, blended between cascades | ADR 0029, 0041 | |
| Performance | Frustum culling; instancing; levels of detail, made at build time; almost no heap allocations per frame; a frame limit on battery | ADR 0031, 0032, 0041, 0042 | |
| Materials | Metallic-roughness PBR with normal, occlusion and emissive maps; transparent, cut-out and double-sided surfaces | ADR 0033, 0034 | [Materials](guides/materials.md) |
| Lights | Point and spot lights: 16 with `forward`, 256 with `forward_plus` | ADR 0037 | [Lighting](guides/lighting.md) |
| Environment | Lighting and reflections from HDR images, drawn as the sky; bloom | ADR 0040 | [Environment](guides/environment.md) |
| Effects | Particles on the CPU: fire, smoke, sparks, one-off bursts | ADR 0038 | [Particles](guides/particles.md) |
| 2D | Sprites, sprite-sheet animation, tile maps with collisions, pixel art kept crisp; parallax backgrounds that repeat, 9-slice sprites, sorting by height for top-down games, animated tiles; levels from the Tiled map editor | ADR 0039, 0049, 0050 | [2D games](guides/sprites.md) |
| Characters | Skeletal animation with fades between clips, skinned on the GPU | ADR 0036 | [Animation](guides/animation.md) |
| Input | Gamepads, with analog sticks and triggers bound to actions | ADR 0035 | [Input](guides/input.md) |

**Missing, in the order planned**

| # | Area | What is missing | Builds on |
|---|---|---|---|
| 1 | 2D | Atlases packed at build time, a renderer module for 2D alone | ADR 0039, 0049, 0050 |
| 2 | Effects | Soft particles, sprite-sheet animation, curves for color and size, trails, effect files shared between scenes; later a `gpu` module, once the graphics interface has compute shaders | ADR 0038 |
| 3 | Lights | Shadows from spot lights, then from point lights; compute shaders in the graphics interface, to sort lights on the GPU | ADR 0037 |
| 4 | Characters | Root motion, animation events, joints sent once per frame for crowds, clips built at build time; then inverse kinematics, blend trees and morph targets | ADR 0036 |
| 5 | Input | Focus navigation in `ui` menus for gamepads, an on-screen aiming cursor for pointer-aimed games such as Toppler, rebinding in a settings menu, a check with a real gamepad on a phone | ADR 0035 |
| 6 | Materials and environment | Reflection probes for interiors, environments prepared at build time, order-independent transparency, antialiased cut-outs, reading `.gltf` files so Blender's images are cooked, half-precision lighting math for phones (PBR costs 1.4 ms of GPU time per frame on the A52s at full screen) | ADR 0033, 0034, 0040 |
| 7 | Levels of detail | Hand-made versions, such as glTF's `MSFT_lod`, taking priority over made ones | ADR 0032, 0042 |
| 8 | Checking | Each rendering feature's pictures on an Adreno, a Mali and an Xclipse phone before it is called done. So far the A52s's Adreno is checked for everything, with Vulkan and OpenGL ES; the A57's Xclipse with Vulkan for the render demo, through environment lighting and bloom (ADR 0040), but not yet the 2D sprites; and no Mali phone yet | All |

### Phase 13: tools for making games

- **Installable SDK:** done (ADR 0045): the SDK is the engine's sources, installed with
  `cmake --install`; `find_package(DreamEngine)`; `dream new <name>` with a project template; and
  games in their own repositories. Still open: a binary SDK if first builds get too slow, and
  `dream` commands that add modules or update a game's `vcpkg.json`.
- **Editor v1:** done (ADR 0046): `dream_editor`, a program built on the engine with its
  interface in Dear ImGui. It has:
  - a scene view, from a camera of its own, where a click selects;
  - an entity tree and an inspector, with components describing their fields through a reflection
    registry, and a game's own components edited by their values' types;
  - saving scenes to TOML, keeping their order, with undo;
  - Play, which builds and starts the game, and sends it each save through hot reload.

  Panels are `editor_panel` modules over `debug_ui`, so a project can add its own. Still open for
  later versions: an asset browser, handles to move entities in the view, editing 2D scenes from a
  2D camera, multi-selection and copying, and field descriptions for the remaining components.
- **Asset database:** done (ADR 0047): the build records every asset, what it needs, its bundle,
  and the platform and cooker it was built for, and fails for a file that names a missing one;
  bundles ship apart, on PC in archives of their own, read through the `asset_source` interface's
  `bundles` module while mounted. Still open: Play Asset Delivery packs on Android, loading bundles
  on a thread of their own, and an asset browser in the editor.
- **Release pipeline:** done (ADR 0048):
  - versions from tags: the engine's from `engine-v<version>`, checked against `CMakeLists.txt`;
    each game's from `v<version>`, in the game, its log, its crash reports, its Windows file
    properties and its Android `versionName` and `versionCode`;
  - release builds that keep their symbols apart from what ships, archived with each release,
    uploaded to Sentry when a game sets it up, and in the Android bundle for Google Play;
  - `<game>_release`, the package or APK and bundle with a symbols archive, and `<game>_aab`;
  - the engine's release workflow, which publishes the SDK, and a release workflow in each game
    from `dream new`, on a self-hosted runner.

  Optional, for the future, when a store or a direct download needs one: **installers.** None are
  made now, since Steam installs from depots, GOG makes its own, and a zip suits itch.io. The
  options, with their licenses:

  | Tool | License | Fits |
  |---|---|---|
  | NSIS | zlib, permissive | A plain `.exe` installer for direct downloads; no conditions at all |
  | Inno Setup | Permissive, zlib-style; since 6.5.0 its makers ask commercial users to buy a license, without requiring it | The same, with a friendlier scripting language |
  | WiX Toolset | Source under MS-RL; using its releases to make money needs its Open Source Maintenance Fee above US$10,000 a year in revenue | MSI packages, which some stores and companies want |
  | MSIX | Microsoft's format | The Microsoft Store; needs a code-signing certificate |

  Each would be a `<game>_installer` target, from the release's package, like `<game>_release`.

### Phase 14: store readiness, Windows and Android

- **Store services interface:** `store_services`, covering achievements, leaderboards, stats, cloud
  saves, purchases and presence. Backends:
  - `null`;
  - `local`, which saves achievements to disk for testing;
  - out-of-tree `steam` and `google_play`.
- **Consent and analytics:** a `consent` interface (`null`, `ump`) and `analytics` (`null`,
  `local_log`, an open backend such as PostHog through HTTP).
- **Crash uploads:** done ahead of the phase: `crash_upload` with `sentry`, `backtrace` and
  `null` (ADR 0043). Left for this phase: a player-facing consent setting, and uploads from Linux.
- **Player-facing basics:** a settings menu template, display modes, localization, accessibility
  options, input remapping.
- **Store build artifacts:** the AAB exists (ADR 0048); left: release signing from a keystore
  outside the repo, Play App Signing, target SDK updates, Play feature checks, Steam depots, and
  code signing for Windows, which SmartScreen warns about without.
- **Linux:** made a tested platform, for Steam Deck.
- **Pass condition:** Toppler, updated, passes Steam Deck Verified checks and the Google Play
  pre-launch report.

### Phase 15: online play

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

### Phase 16: Apple platforms, macOS and iOS

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

### Phase 17: more platforms

- **Consoles** (Xbox, PlayStation, Switch): private, out-of-tree modules under each vendor's
  developer agreement: platform, graphics API, store services. The interfaces built earlier are
  the points they plug into.
- **Web:** Emscripten, with `rhi: webgpu` and `net_transport: websocket`, as an option for demos.
- **Android TV, Chromebooks and foldables,** once gamepads and resizable windows are in.

## 5. Decisions to make before these phases

1. **Proprietary store SDKs:** open, needed before Phase 14. Steamworks, Google Play Billing and
   Play Games Services, and Game Center may be used by a game, but their files can't be published
   in a public repository, and ADR 0022 allows only permissive licenses. Either way the engine
   gets a `store_services` interface with `steam` and `google_play` modules; the choice is where
   those modules' code lives.

   | | A: a private repository outside the engine | B: an optional folder in the engine |
   |---|---|---|
   | How | The modules live in a separate private repository, found through a local path, as console modules will be | Our module code, which calls the SDK, lives in a folder such as `modules/vendor/`. Each developer downloads the SDK, and a CMake option such as `-DDREAM_STEAMWORKS_SDK=<path>` turns the module on; without it the folder isn't built. ADR 0022 gets a written exception: our code may call a platform vendor's SDK, as long as the SDK is optional and never committed |
   | For | The public engine stays fully open source, with no exception to the license rule. No risk of pushing an SDK file by mistake. One approach, shared with consoles | One repository: a change to the interface and to the store modules lands in one commit. The code is visible and reviewable, and others can use it with their own SDK download. Our CI can build it where the SDK is installed |
   | Against | Two repositories to keep in step: a change to `store_services` can break the private modules without the public tests noticing. Public builds can't check the store code compiles. A new machine needs extra setup | An exception to the license rule, guarded by a `.gitignore` entry and a CI check that no SDK file is committed. Public builds still can't compile these modules, so a private CI machine should. A different approach from consoles |

   Consoles stay private (option A) whichever is chosen: console vendors' agreements usually forbid
   publishing even the code that calls their APIs. Option B is the common choice for engines with
   an open core; option A suits keeping the public repository strictly open source, or keeping the
   store integration private.
2. **Editor technology:** decided: an editor hosted in the engine, with its interface in Dear
   ImGui, as the debug UI is. It is quicker to build, draws with the renderer games use, and its
   panels are `editor_panel` modules over `debug_ui`.
3. **Scripting:** decided: none. Games are written in C++, and hot reload covers assets. A
   `scripting` interface can still be added later as a module if a project needs one.
4. **The renderer as a module:** decided and done ([ADR 0030](adr/0030-renderer-module.md)).
5. **Order after Phase 12:** decided: tools (Phase 13), then stores (Phase 14), then online play
   (Phase 15), then Apple platforms (Phase 16), then the remaining platforms (Phase 17).

## 6. Summary

The engine can already ship small, polished 3D and 2D games on Windows and Android phones, built
from modules that swap through configuration, and with the testing, profiling and crash tooling,
crash uploads included, that professional work needs. Before a commercial release on Steam or Google Play, it most needs:
- shadows from point and spot lights, reflection probes, and gamepad navigation in menus;
- a settings menu, localization and store services;
- release packaging: AAB and real signing on Android, depots on Steam;
- an editor beyond scene editing: an asset browser and handles in the view.

Each of those is planned as a module, as is everything for online play, macOS and iOS, so a
project only carries what it uses.
