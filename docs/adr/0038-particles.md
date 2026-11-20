# 0038. Particles

- Status: Accepted
- Date: 2026-11-19

## Context

Games need effects made of many small, short-lived pieces: fire, smoke, sparks, dust, splashes,
hits and magic. The roadmap planned a `particles` interface with a GPU or CPU backend. The graphics
interface has no compute shaders (ADR 0037), so particles can't be moved on the GPU yet, and every
effect has to run on phones as well as PCs.

## Decision

### A `particles` interface, simulated on the CPU

`particles::ParticleSystem` makes emitters from an `EmitterDesc`, moves each one's particles on by
a time step from where a transform places it, and gives them back in world space. Two modules:

| Module | What it does |
|---|---|
| `cpu` | Keeps each emitter's particles in one array and moves them with simple physics: speed, gravity, drag and spin. Color, opacity and size change evenly from birth to death. A seeded generator gives the same particles for the same steps. |
| `null` | Accepts emitters and gives off nothing, for servers and projects without effects. |

An emitter has a rate, a burst at the start of each cycle, a cycle length and whether it loops, a
limit on live particles, and a shape particles are born in: a point, a sphere, a box, or a cone
along its +Y axis. Each particle's lifetime, speed, size, rotation and spin are picked between a
minimum and a maximum. Particles stay where they were born in the world, or with `local_space`
move with their emitter. A one-off effect, such as an explosion, is a burst that doesn't loop.

The module is a few hundred lines with no library: the work is small, and a library would bring
an editor format we don't use.

### Particles as renderer data

`renderer::ParticleBatch` is a span of `ParticleQuad`s (position, size, color, rotation), a texture
and a blend: `Alpha` for smoke and dust, which covers what is behind it by its opacity, or
`Additive` for fire, sparks and magic, which adds its light. `Frame::particles` carries one batch
per emitter. The renderer interface doesn't depend on the particles interface: the runtime copies
the particles across.

### Drawing them in `forward` and `forward_plus`

`particles.slang` draws one square per particle with instancing: the corners come from a small
vertex buffer and each particle from the per-instance data, 48 bytes. The vertex shader turns the
square to face the camera, using the camera's right and up directions sent as push constants. The
fragment shader multiplies the texture, or a soft round dot when there is none, by the particle's
color, and writes premultiplied alpha. Additive particles write an alpha of 0, so one
premultiplied blend draws both kinds, and a batch needs no pipeline change.

Particles are drawn after every surface, blended ones included, in the HDR target, testing depth
without writing it, unlit. Blended batches go farthest first, by their particles' middle, and
their particles farthest first; additive ones go last in any order. A frame draws at most 32,768
particles, about 1.5 MB of the 4 MB of per-frame memory; past that the nearest are kept, and
`FrameStats::particles_dropped` counts the rest.

### The runtime's side

`runtime::ParticleEmitter` is the component: the settings, a texture, the blend, whether it emits,
a time scale, and the particles of the last update. `runtime::Particles` makes the configured
module, keeps one of its emitters per entity, makes a new one when the settings change, and drops
it when the entity goes. `update` moves them on each frame; `warm_up` does so in steps of 1/60 s,
for effects that should already be burning when a scene appears and for screenshot checks, which
need the same particles every run. `burst`, `restart` and `finished` serve one-off effects. Scene
files write `particle_emitter`, with angles in degrees and ranges as a number or `[min, max]`.

Textures are a new asset, `runtime::TextureAsset`, which `Renderer::initialize` registers: a PNG,
or the cooked KTX2 beside it when the build made one, so particle textures in a `textures` folder
are compressed like models' and reload when changed.

## Consequences

- Effects are data in scene files, and the render demo has a campfire: additive flames and sparks,
  textured smoke and a point light. The demo's screenshot checks warm the effects up and hold them,
  and match on all five PC backends; on a phone the effects look the same with Vulkan and
  OpenGL ES.
- Particles are unlit, and cast and receive no shadows. They don't collide with the world.
- Particles are sorted per batch, so two blended effects that overlap in depth can draw in the
  wrong order where they cross; additive effects never show it.
- Hard edges show where a particle crosses a surface: there are no soft particles, which need the
  depth buffer as a texture while drawing.
- Every effect is moved on the CPU each frame: a few thousand particles cost well under a
  millisecond, but tens of thousands want the GPU, once the graphics interface has compute shaders.
- No sprite-sheet animation, trails, colors or sizes following curves with more than two points,
  or effect files shared between scenes yet. Each is a later step on the roadmap.
