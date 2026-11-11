# 0014. A second backend for physics, audio and interfaces, and in-house libraries

- Status: Accepted
- Date: 2026-11-10

## Context

Every interface should have two real implementations (ADR 0001, rule 3), and a project should be
able to switch between them by editing its module lists. Physics, audio and the game interface each
had one: Jolt, Box2D, miniaudio and RmlUi. Third-party code must be under the MIT or Apache 2.0
license, so where no such library does the job, the engine writes its own.

## Decision

### Backends

| Interface | Backend | Built on | License |
|---|---|---|---|
| `physics2d` | `chipmunk` | Chipmunk2D 7 | MIT |
| `physics3d` | `impulse` | `libraries/impulse`, in-house | the engine's |
| `audio` | `portaudio` | PortAudio for output, dr_libs for decoding, `libraries/mixer`, in-house | MIT; MIT-0 or public domain |
| `ui` | `markup` | `libraries/markup`, in-house, with stb_truetype | the engine's; MIT or public domain |

Why in-house code was needed:

- **3D physics.** Jolt is the only MIT or Apache 2.0 C++ 3D physics engine of its kind. Bullet,
  ReactPhysics3D and qu3e are zlib, PhysX is BSD-3, and ODE is LGPL or BSD.
- **Mixing.** PortAudio only plays samples. The mixers that exist are zlib (SoLoud), LGPL
  (OpenAL Soft) or miniaudio itself, which already is a backend.
- **Game interfaces.** Switching by configuration alone means the second backend must read the same
  RML and RCSS files as RmlUi, and no MIT or Apache 2.0 library does. Immediate-mode libraries such
  as Nuklear or microui can't load documents at all.

dr_libs is offered as MIT No Attribution or public domain, which is more permissive than MIT; the
engine already uses it inside miniaudio.

### In-house libraries are a layer of their own

`libraries/<name>/` holds code the engine owns but that doesn't depend on it, so it could be used by
any program. A library:

- uses only the C++ standard library and third-party libraries, never Core or an interface; the
  layer check enforces this;
- is used only by backends, which adapt it to an interface, as with any third-party library;
- is built with the engine's settings by `dream_add_library`, as `dream::library_<name>`, and is
  always built, so its tests run even when no sample selects a backend that uses it.

This keeps each backend small: its module only translates between the interface and the library.

### impulse

A sequential-impulse solver with warm starting, a separate position-correction pass and speculative
contacts, as in Box2D 2.4, carried over to 3D. Boxes collide by the separating axis test, with the
contact face clipped to up to four points. Spheres and capsules are treated as segments with a
radius. Contacts are solved in alternating order each iteration, which lets stacks settle with ten
iterations. Touching bodies sleep together as islands. Settings are under `[physics3d.impulse]`.

### portaudio

The device callback calls the mixer, which resamples each voice by linear interpolation. The mixer
takes a lock for each block it mixes, and the game's calls take the same lock. On Windows, the
default outputs of WASAPI, DirectSound and MME are tried in that order, and the first that opens is
used: MME, PortAudio's own default, has a delay that can be heard. WASAPI streams ask Windows to
convert them (`paWinWasapiAutoConvert`), because its shared mode otherwise only takes the exact
format the device mixes in, which is often not stereo. Debug builds of PortAudio, such as vcpkg's,
print a trace to stderr; on Windows the module sends it to the engine's log at the debug level.
`[audio.portaudio] device = false` mixes only when asked, as miniaudio's setting does, so tests can
check the samples.

### markup

It reads RML and RCSS as RmlUi does, including its quirks. Elements are inline unless a style sheet
says otherwise, `dp` units follow the display scale, inputs get their type as a class, and sliders
have `slidertrack` and `sliderbar` parts to style. It covers block, inline, inline-block and
absolute layout, rounded boxes, borders, text with wrapping, selectors with the usual
pseudo-classes, and buttons, sliders, checkboxes and text fields. It has no global state, so several
interfaces can exist at once.

### Checking that backends are interchangeable

Each new backend passes the shared conformance tests its interface has. The physics and interface
backends also pass tests built from the samples: the physics demo's and arcade demo's scenes and
scripts, and the arcade demo's own interface files. Two more CTests on Windows,
`dream_physics_demo_alternatives_check` and `dream_arcade_demo_alternatives_check`, run the samples'
scripted checks with only the new backends selected and expect the same outcome.

The samples list each new backend after the existing one for Windows.

## Consequences

- Chipmunk has no continuous collision, so `continuous_collision` has no effect there. Its joint
  limits are corrected gently, because its corrections leave velocity behind.
- impulse has no meshes, convex hulls or height fields, like the interface. Stacks taller than about
  twelve boxes need more iterations.
- markup has no scrolling, animation, transitions, transforms, tables, flexbox or decorators, and
  slider arrows are not drawn. Documents that use them lay out in a simpler way than with RmlUi.
- PortAudio doesn't support Android, and vcpkg installs it, and Chipmunk, only for other platforms.
  The Android module lists are unchanged.
- Three in-house libraries are now the engine's to maintain.
