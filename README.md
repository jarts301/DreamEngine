# DreamEngine — offline Linux web build

> **Never merge this branch into `main`.** See [OFFLINE_WEB_BRANCH.md](OFFLINE_WEB_BRANCH.md).

A modular C++20 game engine, on the `offline-web` branch: a version of DreamEngine for a Linux
machine with **no internet, no GPU, no display, no sound card, no Docker and no package manager**.
Everything it needs is in this repository, and every game, tool and test is used from a **web
browser**. See [ADR 0039](docs/adr/0039-offline-linux-web-build.md) for why and how, and the `main`
branch for the full engine (Windows, Android, Vulkan, Direct3D, OpenGL).

Architecture rules and conventions are in [docs/adr](docs/adr/README.md).

## What it needs

| Tool | Version | Notes |
|---|---|---|
| Linux | x86-64 or arm64 | Tested on Ubuntu 24.04 |
| A C and C++20 compiler | GCC 11+ or Clang 14+ | Tested with GCC 13 |
| CMake | 3.22 or newer | Tested with 3.28; uses `make`, Ninja is optional |
| A web browser | Any recent one | On the same machine, or on another one that can reach it |

Nothing is downloaded at any point: no vcpkg, apt-get, Docker, Go modules, NuGet, Ruby gems or
Gradle. The third-party libraries are kept, with their licenses, in
[third_party/](third_party/README.md). Python is not needed.

## Build and test

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure --parallel 8
```

A build without `CMAKE_BUILD_TYPE` is a release build, since the software renderer is slow
unoptimized. `cmake --preset linux-release` (or `linux-debug`) does the same with a preset.

## Use it from the browser: the hub

```bash
build/tools/hub/dream_hub
```

Then open <http://127.0.0.1:8000/>. The hub's page has four tabs:

- **Games**: start any sample game, open its page to play it, stop it, read its log.
- **Tests**: run the whole test suite (or the tests matching a filter), follow its output, and read
  each test's result and output.
- **Screenshots**: the frames the screenshot tests captured, beside their reference images.
- **Build**: rebuild everything after changing the code, and read the compiler's output.

`dream_hub --host 0.0.0.0 --port 8000` lets other machines on a trusted network open it. The hub has
no login: anyone who can reach it can build, start the listed games and run the tests, and nothing
else.

## Play a game without the hub

Each game serves its own page. Run it from its folder and open the address it prints:

```bash
build/samples/toppler/dream_toppler/dream_toppler
# Open http://127.0.0.1:8080/ in a web browser to see and play Toppler
```

Click the picture to give it the keyboard. The mouse, wheel, keyboard and touch work as on a
desktop. *Stop game* ends the program. `DREAM_WEB_PORT` and `DREAM_WEB_HOST`, or
`[platform.web] port` and `host` in the game's `dream.toml`, choose where it listens.

| Sample | What it shows |
|---|---|
| `toppler` | A 10-level knockdown puzzle game: physics, interface, saved progress |
| `arcade_demo` | Throwing balls at targets: physics, interface, debug tools (F1) |
| `physics_demo` | 3D physics with impulse, and 2D physics with Box2D (Tab switches) |
| `render_demo` | Materials, normal maps, shadows, point and spot lights, skinned animation, particles |
| `hello_game` | The smallest game: a window, input actions and saved files |
| `module_demo` | Choosing a module from a list, without a game loop |

## How a game is drawn and shown

The engine's interfaces are unchanged; this branch only adds and removes backends (modules), as a
project's `dream.toml` selects them:

| Interface | Backends here | Notes |
|---|---|---|
| `platform` | **`web`**, `null` | The window is a web page the game serves itself; input comes back from it |
| `rhi` | **`software`**, `null` | Resources live in memory; frames go to the web page and to screenshot tests |
| `renderer` | **`software`**, `null` | Draws the scene on the CPU, on every core |
| `physics3d` | `impulse`, `null` | |
| `physics2d` | `box2d`, `null` | |
| `animation` | `ozz`, `null` | |
| `ui` | `markup`, `null` | |
| `debug_ui` | `imgui`, `null` | |
| `audio` | `null` | No sound card: sounds are tracked but never heard |
| `input_device` | `null` | No gamepads |
| `particles` | `cpu`, `null` | |
| `mesh_simplifier`, `tangent_generator` | `meshoptimizer`, `mikktspace`, `null` | |
| `profiler` | `timeline`, `null` | Profiles open at <https://ui.perfetto.dev>, or any Chrome trace viewer |

The software renderer draws what the GPU renderer of the main branch draws, with the same lighting:
metallic-roughness materials, normal maps, the sun with a shadow map, point and spot lights,
masked and blended surfaces, skinned parts, particles, debug lines and the interface, then exposure
and PBR Neutral tone mapping. Its settings, in `dream.toml`:

```toml
[renderer]
exposure = 1.0
tone_mapping = "neutral"     # or "none"
max_lights = 16
culling = true

[renderer.software]
threads = 8                  # default: one per core, up to 16

[renderer.shadows]
mode = "auto"                # or "off"
resolution = 1024
distance = 40.0
```

The samples draw at 960x540 and at most 30 frames a second, which a few cores keep up with. Raise
`[app] width`, `height` and `max_fps` on a faster machine.

## Tests

`ctest` runs the engine's unit tests, each library's and backend's tests, and the samples run
start to finish without a person: Toppler plays all ten levels, the arcade and physics demos play
scripted runs, and the screenshot tests compare frames of the software renderer with the reference
images in `samples/*/tests/reference/screenshots` (ADR 0021). A screenshot test that fails saves
its capture in its build folder, and the hub shows it beside the reference. To accept a new picture
after a deliberate change, copy the capture over the reference.

The web page itself is tested by `dream_platform_web_tests`, which serves it, posts input to it
and reads frames from it, as a browser does.

## Packaging

`cmake --build build --target dream_toppler_package` puts the game and its assets, packed into
`assets.pak`, in `build/samples/toppler/dream_toppler_package/`, ready to copy to another Linux
machine.

## Layout

| Folder | Contents |
|---|---|
| `core/` | Logging, results, handles, allocators, math, configuration, jobs, crash reports |
| `interfaces/` | What each kind of module provides |
| `modules/` | The backends, one folder each |
| `libraries/` | In-house libraries the backends build on: `impulse`, `markup`, `mixer`, `timeline`, `webserve` |
| `runtime/` | The game loop, world, assets, scenes, renderer front end |
| `samples/` | The games above |
| `third_party/` | Everything from outside, with licenses |
| `tools/` | `hub` (the web page) and `pack` (asset archives) |
| `docs/` | Decisions (ADRs), guides and reports |
