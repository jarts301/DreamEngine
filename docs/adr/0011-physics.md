# 0011. Physics

- Status: Accepted
- Date: 2026-11-09

## Decision

### Two interfaces

`physics3d` and `physics2d` are separate interfaces, as planned in ADR 0001: 2D engines work
differently and 2D games need different shapes, so one interface would fit neither well. They
mirror each other where they can, so code and habits carry over.

Each one has bodies (static, kinematic, dynamic), box, sphere or circle, and capsule shapes,
sensors, joints (fixed, point in 3D, hinge, distance), ray casts, overlap queries and contact
events. Units are meters, kilograms and seconds, with +Y up. Hinge angles are always of `body_a`
relative to `body_b`, so the backends, which measure the other way round, swap the bodies.

The rules for a valid body are inline functions in the interface headers, so every backend
accepts and rejects the same descriptions.

### Backends

- **Jolt** for 3D, with one worker thread per spare core, up to four, and limits set under
  `[physics3d.jolt]`.
- **Box2D** for 2D. Its own rotation helpers are approximate, so the backend uses exact sine,
  cosine and atan2.
- **Null** backends for both, which move bodies by velocity and gravity without collisions, for
  runs with no physics and for testing engine code.

Shared conformance tests in `modules/physics3d/tests` and `modules/physics2d/tests` hold every
backend to the same behavior, and the real backends also pass collision tests.

### Fixed timestep

The game loop now runs `Game::on_fixed_update` at `[app] fixed_rate` times per second (60 by
default), before `on_update`. A slow frame runs up to eight steps and drops the rest, so a slow
device slows the simulation down instead of falling further behind.

### Connecting to the ECS

`runtime::Physics3D` and `runtime::Physics2D` turn components into bodies. A game adds
`RigidBody3D` or `Joint3D` (or the 2D versions), in code or through the scene components
`rigid_body`, `joint`, `rigid_body_2d` and `joint_2d`, and calls `step` from `on_fixed_update`.
Bodies are created and removed to match the components on each step, so games never handle bodies
directly.

Dynamic bodies write their pose back to the Transform, interpolated between the last two steps by
`App::fixed_step_alpha`, so motion stays smooth at any frame rate. Kinematic bodies follow their
Transform, and static bodies stay where they started. Bodies must be on entities without a parent,
because their Transforms are treated as world space.

Contacts are published on the App's event bus as `Contact3D` and `Contact2D`, naming entities.

### Debug drawing

`Physics3D::debug_draw` outlines every body, colored by kind, plus joints and new contacts, into
the renderer's `DebugLines`. Drawing is done by the runtime from the shapes it already knows, so it
looks the same with every backend. Lines are drawn after the scene without depth testing.

For this the graphics interface gained a line topology, an option to skip depth writes, and
`draw_vertices`, which copies vertices into per-frame memory (2 MB per frame).

### Building dependencies with the engine's compiler

Jolt is the first compiled C++ dependency, and vcpkg built it with the newest Visual Studio on the
machine. MSVC can't link a library built by a newer version of itself, so the Windows presets now
use an overlay triplet, `cmake/triplets/x64-windows-static-md.cmake`, which builds dependencies
with the Visual Studio and MSVC version of the active environment.

## Consequences

- There are no mesh, convex hull or height field shapes, collision layers, or character
  controllers yet.
- Changing a body's description after it exists has no effect until the component is replaced.
- Bodies under a parent entity are refused.
- Only the active scene's physics advance in the sample; each `Physics3D` or `Physics2D` simulates
  one World.
- Contact events from Jolt arrive from worker threads and are collected under a lock, so they are
  delivered after the step, not during it.
