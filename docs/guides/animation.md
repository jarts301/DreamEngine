# Animation: skeletons, skins and clips

This guide explains how to make animated models for DreamEngine, how to play their animations
from scene files and code, and what to check when they look wrong. The design is in
[ADR 0036](../adr/0036-skeletal-animation.md).

## In short

- Animated models are glTF files with an **armature**: a skeleton of joints, a mesh **skinned** to
  it, and one or more **animations**, which the engine calls clips.
- List `animation = ["ozz", "null"]`, create a `runtime::Animation` in `on_start`, and call its
  `update` every frame before rendering.
- Give an entity an `animator` with a clip's name, or call `animation.play(...)` to change clips
  with a smooth fade.
- At most **4 joints per vertex** and **256 joints per skinned mesh**.

## Setting up

```toml
[modules.windows]
platform = ["sdl3"]
rhi = ["vulkan", "directx12", "directx", "opengl"]
renderer = ["forward", "null"]
animation = ["ozz", "null"]

[modules.android]
platform = ["sdl3"]
rhi = ["vulkan", "opengles"]
renderer = ["forward", "null"]
animation = ["ozz", "null"]
```

| `animation` | What it does |
|---|---|
| `ozz` | Plays animation with ozz-animation (MIT): compressed clips, sampled and blended four joints at a time. |
| `null` | Leaves every model in its rest pose. |

```cpp
class MyGame final : public runtime::Game {
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        // Before loading scenes that have animators.
        if (auto ready = m_animation.initialize(app); !ready.has_value()) {
            return ready;
        }
        // ...load the world.
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        m_world->update_transforms();
        m_animation.update(*m_world, time.delta_seconds);
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size());
    }

    runtime::Renderer m_renderer;
    runtime::Animation m_animation;
    std::unique_ptr<runtime::World> m_world;
};
```

A skinned model drawn without an `Animator`, or in a game without `Animation`, stands in its rest
pose, the pose it was bound in.

## Playing clips

From a scene file:

```toml
[entities.hero.mesh]
model = "assets:/models/hero.glb"

[entities.hero.animator]
clip = "idle"        # The animation's name in the glTF file.
speed = 1.0          # Optional. 0 holds the pose; negative plays backward.
loop = true          # Optional. Otherwise it stops on its last pose.
playing = true       # Optional.
time = 0.0           # Optional. Where in the clip to start, in seconds.
```

From code:

```cpp
// Fade from whatever plays to "run" over 0.2 seconds.
if (auto played = m_animation.play(*m_world, hero, "run", 0.2f); !played.has_value()) {
    DREAM_LOG_WARNING("{}", played.error().message);   // The model has no such clip.
}

// A one-off that stops on its last pose.
m_animation.play(*m_world, hero, "jump", 0.1f, /*loop=*/false);

// Read or change it directly.
runtime::Animator& animator = m_world->registry().get<runtime::Animator>(hero);
animator.speed = sprinting ? 1.5f : 1.0f;
if (!animator.playing) {        // A clip that doesn't loop has finished.
    m_animation.play(*m_world, hero, "idle");
}
```

`Animation::clip_names(model)` lists a model's clips, and `Animation::duration(model, "run")` says
how long one lasts.

**Fades:** `play` blends the last clip out while the new one comes in, both moving on. 0.1 to
0.3 seconds suits most changes: shorter for snappy actions such as attacks, longer for walking into
running. With 0 the change is instant.

**Keeping clips in step:** to change between walk and run without the feet jumping, make both
clips the same length in steps, and start the new one at the matching time by setting
`animator.time` after `play`.

## Making animated models

### In Blender

1. **Model** the mesh in its rest pose. A T-pose or A-pose is usual for characters.
2. **Apply the object's scale and rotation** (*Object → Apply → All Transforms*) on both the mesh
   and the armature before binding. Unapplied scale is the most common cause of a model that
   explodes or shrinks when it animates.
3. **Add an armature** and parent the mesh to it *With Automatic Weights*, then fix the weights in
   *Weight Paint* mode.
4. **Limit each vertex to 4 joints:** *Weights → Limit Total* (limit 4), then *Weights → Normalize
   All*. The exporter limits them too, but doing it yourself shows you what you get.
5. **Animate** in the *Action Editor*: one action per clip, named as the game will play it:
   `idle`, `walk`, `run`. The glTF exporter writes each action as an animation.
6. Stay within **256 bones** that deform the mesh. Bones that only control others (IK targets,
   poles) can be left out of the export with *Deform Bones Only*.

Export with *File → Export → glTF 2.0*:

| Setting | Value | Why |
|---|---|---|
| Format | glTF Binary (.glb) | The engine reads `.glb`. |
| Data → Mesh → Apply Modifiers | On, but not the Armature modifier | The skin must stay a skin. |
| Data → Armature → Use Rest Position | On | Binds the mesh as it rests. |
| Data → Armature → Export Deformation Bones Only | On | Leaves out control bones. |
| Animation → Mode | Actions | Every action becomes a clip. |
| Animation → Sampling Animations | On, rate 1 | Bakes constraints and IK into keys the engine plays. |
| Animation → Shape Keys | Off | Morph targets aren't supported. |

The engine plays keys linearly, as most exporters write them. Step keys and cubic-spline curves
are supported too, turned into linear keys as the model loads.

### Props held by a character

A mesh that isn't skinned but is parented to a bone, such as a sword parented to the hand, follows
that bone. In Blender, parent the object to the bone (*Parent → Bone*) rather than skinning it.

### Checking a model

`samples/render_demo` has a tentacle, skinned to five joints, with clips "sway" and "curl". Press
N, or north on a gamepad (Y on Xbox, Triangle on PlayStation), to fade between them. Put a model in
`samples/render_demo/assets/scenes/yard.toml` with an `animator` to see it beside the tentacle;
hot reload picks up a re-exported model while the game runs.

[samples/animation](../../samples/animation) plays the tentacle's clips seven ways: at half and
twice the speed, backward with a negative speed, once with `loop = false`, and fading between clips
with `Animation::play` every two seconds.

| You see | Likely cause | Fix |
|---|---|---|
| The model stands still in its rest pose | No `Animator`, a misspelled clip name, `Animation` not created or not updated, or `animation` not listed | Check the log for the module in use, and the names `Animation::clip_names` gives |
| It explodes, stretches or shrinks when it moves | Unapplied object scale or rotation before binding | Apply all transforms on mesh and armature, rebind, re-export |
| Parts of it stay behind while the rest moves | Vertices with no weights, or weights to bones not exported | Weight every vertex; export deformation bones only after checking none of them deform |
| It loads with "a skinned vertex names joint N" | A weight to a bone that isn't in the skin | Re-export with the armature's deform bones, or clean the weights |
| It loads with "a skin has N joints" | More than 256 deforming bones | Merge or remove bones; split the character into parts |
| It looks rigid at the elbows and knees | Too few weights blending across the joint | Smooth the weights around joints |
| Feet slide or pop when changing clips | Clips not in step, or a fade too long | Match lengths and start times; shorten the fade |

## Performance

Each skinned part of each character is its own draw, in every pass that draws it: the scene, and
each shadow cascade it falls in. Its joints are sent with each draw. A few dozen characters are
fine. For crowds:

- **Keep characters in one part** where you can: one material and one mesh means one draw per pass.
- **Keep joint counts down.** 30 to 70 joints is typical for a character; fingers and faces add
  many.
- Every animator is posed every frame, however far away: there is no lower update rate for
  distant characters yet.

## Limits

Each of these is a later step on the roadmap:

- Root motion: clips that move the character's position, for feet that don't slide.
- Animation events, such as a footstep sound at a given moment.
- Inverse kinematics, blend trees and state machines, and additive layers.
- Morph targets (shape keys), for faces.
- Ragdolls.
- Building clips at build time; they are built as models load.

## How it works inside

1. **`runtime::parse_glb`** reads the skeleton (every joint and every node above one), each skin's
   joints and inverse bind matrices, and each animation, into `ModelData`.
2. **`runtime::create_model_asset`** keeps them in the `ModelAsset`, with each skin's matrices at
   rest and a box per joint for each skinned part.
3. **`runtime::Animation`** builds each model's skeleton and clips in the `animation` module once,
   and each `update` poses every `Animator` and stores each skin's matrices in it.
4. **`runtime::Renderer`** gives each skinned part its matrices, as `MeshDraw::joint_matrices`, and
   a box made from its joints' boxes as posed.
5. **The `forward` module** draws skinned parts with the `_skinned` shaders, which move each
   vertex by its four joints before its instance's transform. The joints arrive in the per-draw
   uniform block, `CommandList::set_draw_uniforms`.
