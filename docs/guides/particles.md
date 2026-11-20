# Particles: fire, smoke, sparks and other effects

This guide explains how to add particle effects to a DreamEngine game, what each setting does, and
how to make common effects look right. The design is in [ADR 0038](../adr/0038-particles.md).

## In short

- An **emitter** gives off **particles**: small squares that always face the camera, each living a
  short time, moving, and changing color, opacity and size as it ages.
- List `particles = ["cpu", "null"]`, create a `runtime::Particles` in `on_start` after the
  renderer, and call its `update` every frame after `update_transforms`.
- Give an entity a `particle_emitter` in a scene file, or a `runtime::ParticleEmitter` in code. The
  emitter is placed and turned by its entity.
- Use `blend = "additive"` for things that glow (fire, sparks, magic) and the default `"alpha"` for
  things that hide what is behind them (smoke, dust).
- Particles are unlit, cast no shadows and don't collide. At most 32,768 are drawn in a frame.

## Setting up

```toml
[modules.windows]
renderer = ["forward_plus", "forward", "null"]
particles = ["cpu", "null"]

[modules.android]
renderer = ["forward_plus", "forward", "null"]
particles = ["cpu", "null"]
```

| `particles` | What it does |
|---|---|
| `cpu` | Moves particles on the CPU. A few thousand cost well under a millisecond on a phone. |
| `null` | Gives off nothing, for a server or a build without effects. Scenes still load. |

```cpp
class MyGame final : public runtime::Game {
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        // After the renderer, which loads particle textures, and before loading scenes.
        if (auto ready = m_particles.initialize(app); !ready.has_value()) {
            return ready;
        }
        // ...load the world.
        m_world->update_transforms();
        m_particles.warm_up(*m_world, 2.0);   // Optional: fires already burning.
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        m_world->update_transforms();
        m_particles.update(*m_world, time.delta_seconds);
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size());
    }

    runtime::Renderer m_renderer;
    runtime::Particles m_particles;
    std::unique_ptr<runtime::World> m_world;
};
```

## Adding an emitter

In a scene file, every setting is optional:

```toml
[entities.torch.transform]
position = [0.0, 1.2, 0.0]

[entities.torch.particle_emitter]
blend = "additive"
shape = "cone"
radius = 0.1
rate = 50.0
lifetime = [0.4, 0.8]
speed = [0.5, 1.0]
size = [0.2, 0.3]
end_size = 0.3
start_color = [1.6, 0.6, 0.16]
end_color = [0.9, 0.12, 0.02]
end_alpha = 0.0
```

In code, the same settings are a `particles::EmitterDesc`, with angles in radians:

```cpp
const runtime::Entity torch = world.create_entity();
world.registry().get<runtime::Transform>(torch).position = core::Vec3{0.0f, 1.2f, 0.0f};
world.registry().emplace<runtime::ParticleEmitter>(torch, runtime::ParticleEmitter{
    .desc = {.rate = 50.0f,
             .shape = particles::EmitterShape::Cone,
             .extents = {0.1f, 0.0f, 0.0f},
             .lifetime = {0.4f, 0.8f},
             .speed = {0.5f, 1.0f},
             .size = {0.2f, 0.3f},
             .end_size = 0.3f,
             .start_color = {1.6f, 0.6f, 0.16f, 1.0f},
             .end_color = {0.9f, 0.12f, 0.02f, 0.0f}},
    .blend = renderer::ParticleBlend::Additive});
```

Change any of the `desc` settings while the game runs and the emitter starts again with them.
`emitting = false` stops new particles while those alive finish; `time_scale` slows, speeds or,
at 0, freezes an effect.

## The settings

| Setting | Default | What it does |
|---|---|---|
| `texture` | none | An image with straight alpha, multiplied by each particle's color. Without one, each particle is a soft round dot. Keep it in a `textures` folder so the build compresses it. |
| `blend` | `"alpha"` | `"alpha"` covers what is behind by the particle's opacity; `"additive"` adds its light and never darkens. |
| `emitting` | true | Whether new particles are born. |
| `time_scale` | 1 | How fast time passes for the effect. |
| `rate` | 10 | Particles born per second. |
| `burst` | 0 | Particles born at once when the emitter starts, and at the start of each cycle. |
| `duration` | 0 | Seconds per cycle; 0 runs forever. |
| `loop` | true | Start another cycle at the end of one. Off with a duration of 0: only the burst, for a one-off. |
| `max_particles` | 256 | The most alive at once, up to 65,536. None are born while there are this many. |
| `shape` | `"point"` | Where particles are born and which way they leave; see below. |
| `radius` | 0 | For `"sphere"` and `"cone"`. The same as `extents`' first value. |
| `extents` | [0, 0, 0] | For `"box"`: half its size along X, Y and Z. |
| `cone_angle` | 23 | For `"cone"`: degrees from the emitter's +Y axis that particles may leave at. |
| `lifetime` | 1 | Seconds each particle lives. |
| `speed` | 1 | Meters per second as it is born. |
| `size` | 0.1 | Meters across as it is born. |
| `end_size` | 1 | Its size is multiplied by this as it dies: above 1 grows, below 1 shrinks. |
| `rotation` | 0 | Degrees it starts turned by, around the view direction. |
| `spin` | 0 | Degrees per second it turns. |
| `start_color`, `start_alpha` | [1, 1, 1], 1 | Linear color and opacity as it is born. Colors above 1 glow brighter in HDR. |
| `end_color`, `end_alpha` | [1, 1, 1], 0 | As it dies. In between they mix evenly. |
| `gravity` | [0, 0, 0] | Acceleration in meters per second squared: `[0, -9.8, 0]` falls, a small upward value rises like hot smoke. In the world's axes, or the emitter's with `local_space`. |
| `drag` | 0 | How quickly it slows: each second its speed is multiplied by e^-drag. 1 loses about two thirds a second. |
| `local_space` | false | Particles move with the emitter, as a jet's flame; otherwise they stay where they were born, as a trail of smoke behind a moving torch. |
| `seed` | 1 | Two emitters with the same settings and seed give the same particles. Give neighbors different seeds. |

`lifetime`, `speed`, `size`, `rotation` and `spin` take one number, or `[min, max]` to pick each
particle's at random between them.

**Shapes:**

| Shape | Born | Leaving |
|---|---|---|
| `point` | At the emitter | Every way |
| `sphere` | Inside a ball of `radius` | Outward from its middle |
| `box` | Inside a box of half-size `extents` | Every way |
| `cone` | On a disc of `radius` across the emitter's XZ plane | Up its +Y axis, within `cone_angle` |

Turn the entity to point a cone: `rotation = [90.0, 0.0, 0.0]` sends it along +Z, sideways.

## Making effects

Starting points, to tune by eye.

| Effect | Blend | Shape | Rate | Lifetime | Speed | Size → end_size | Colors | Other |
|---|---|---|---|---|---|---|---|---|
| Flames | additive | cone, radius 0.2, 15° | 60–100 | 0.5–0.9 | 0.6–1.1 | 0.4–0.6 → 0.2 | [1.6, 0.6, 0.16] → [0.9, 0.12, 0.02], alpha to 0 | gravity [0, 0.8, 0] |
| Sparks | additive | cone, 25° | 10–20 | 0.9–1.5 | 2–3 | 0.05 → 0.5 | [4, 2, 0.6] → [2, 0.4, 0.05], alpha to 0 | gravity [0, -3, 0], drag 0.6 |
| Smoke | alpha, textured | cone, 12° | 5–10 | 3.5–4.5 | 0.5–0.7 | 0.6–0.8 → 4 | grey, alpha 0.8 → 0 | spin ±25, drag 0.3, a little sideways gravity for wind |
| Dust puff | alpha, textured | sphere, radius 0.3 | burst 20, loop off | 0.8–1.2 | 0.5–1.5 | 0.3 → 2 | sand color, alpha 0.5 → 0 | drag 2, gravity [0, -0.5, 0] |
| Explosion | additive | sphere | burst 60, loop off | 0.3–0.6 | 4–8 | 0.3 → 0.1 | [6, 3, 1] → [1, 0.2, 0], alpha to 0 | drag 3 |
| Magic sparkles | additive | sphere, radius 0.5 | 30 | 1–2 | 0.1–0.3 | 0.04 | [0.6, 1.5, 3] → [0.3, 0.2, 2], alpha to 0 | gravity [0, 0.3, 0] |
| Falling snow | alpha | box, extents [10, 0, 10], high above the camera | 200 | 8–10 | 0 | 0.05 | white, alpha 1 → 1 | gravity [0, -1, 0], drag 1, max_particles 2000 |

**Tips:**

- **Glow comes from additive blending and colors above 1.** A flame color of [1.6, 0.6, 0.16]
  brightens what is behind it; stacked flames make a hot core. Fade alpha to 0 rather than the
  color to black for a soft end.
- **Fade everything out.** An `end_alpha` of 0 hides the moment particles die. Without it,
  particles pop out.
- **Add a light.** A `point_light` beside a fire lights the ground around it, which particles
  can't, being unlit.
- **Layers make effects.** A campfire is three emitters on child entities of one: flames, sparks
  and smoke, as in the render demo's yard. Move or remove the parent and they all follow.
- **Smoke textures:** a white, lumpy, soft-edged puff on a transparent background, 64 to 128
  pixels across. The particle's color tints it. `tools/make_particle_textures.py` makes the demo's.
- **Random rotation and spin** stop a textured effect from looking like copies of one image.

### One-off effects

A hit, an explosion or a puff of dust happens once. Make an emitter with a burst, a duration of 0
and `loop = false`, on a new entity where it happens, and remove the entity once
`Particles::finished` says it has ended:

```cpp
const runtime::Entity boom = world.create_entity();
world.registry().get<runtime::Transform>(boom).position = hit_point;
world.registry().emplace<runtime::ParticleEmitter>(boom, explosion);   // A ParticleEmitter kept
m_effects.push_back(boom);                                             // for reuse.

// Each frame, after Particles::update:
std::erase_if(m_effects, [&](runtime::Entity effect) {
    if (!m_particles.finished(*m_world, effect)) {
        return false;
    }
    m_world->destroy_entity(effect);
    return true;
});
```

`Particles::burst(world, entity, count)` adds particles to any emitter at once, such as a burst of
sparks from a torch when it is hit; `Particles::restart` starts an emitter over.

## Performance

- **Count particles, not emitters.** Each live particle costs CPU time to move each frame, and
  covers pixels on screen. The profiler shows "Particles" drawn and "Particles alive".
- **Large particles near the camera are the cost on phones**: every pixel they cover is drawn once
  per particle over it. A screen full of overlapping smoke can cost more than the whole scene.
  Keep near smoke to fewer, more transparent particles.
- **Keep `max_particles` near what the effect needs**, so a long frame can't produce a flood.
- Particles out of view are still moved on, and drawn: there is no culling of effects yet.

## Checking

`samples/render_demo` has a campfire in the front right of the yard: additive flames and sparks,
textured smoke drifting with a little wind, and a point light. Its screenshot checks set
`demo.particle_time`, which warms the effects up for that many seconds and then holds them.

| You see | Likely cause | Fix |
|---|---|---|
| Nothing | `particles` not listed, `Particles` not created or updated, the entity has no transform, or `update_transforms` isn't called first | Check the log for the module in use |
| A scene with an emitter doesn't load, naming the texture | `Particles` initialized before the `Renderer`, or a wrong path | Initialize the renderer first; check the path |
| Square particles | No texture, and you wanted one, or the texture has no alpha | Give it a PNG whose alpha is soft at the edges |
| Smoke glows | It is additive | Use `blend = "alpha"` |
| Fire looks dull and grey | It is alpha-blended, or its colors are at most 1 | Use `blend = "additive"` and colors above 1 |
| Particles pop out at the end | `end_alpha` is above 0 | Set `end_alpha = 0` |
| A hard line where particles meet the ground | Particles crossing a surface are cut by it | Raise the emitter, or shrink the particles near the ground |
| Effects look the same next to each other | Same settings and seed | Give each a different `seed` |
| Some particles missing in busy scenes | More than 32,768 in a frame | Lower rates, lifetimes or `max_particles` |

## Limits

Each of these is a later step on the roadmap:

- Moving particles on the GPU, for tens of thousands, once the graphics interface has compute
  shaders.
- Soft particles that fade where they meet surfaces.
- Lit particles, and particles casting shadows.
- Sprite-sheet animation, such as a flickering flame drawn in frames.
- Colors and sizes following curves with more than two points.
- Trails and ribbons.
- Collisions with the world.
- Effect files shared between scenes, and an editor.

## How it works inside

1. **`runtime::Particles`** keeps one emitter of the `particles` module for each entity with a
   `ParticleEmitter`, and each `update` moves it on from the entity's world transform and copies
   its particles into the component.
2. **The `cpu` module** moves each particle by its speed, gravity and drag, removes the dead,
   gives off new ones at the emitter's rate, and works out each one's color and size from its age.
3. **`runtime::Renderer`** gives each emitter's particles, its texture and its blend to the
   renderer module as a `ParticleBatch`.
4. **The `forward` module** sorts blended batches and their particles farthest first, and draws
   each batch as instanced squares with `particles.slang`, facing the camera, after every surface
   of the scene.
