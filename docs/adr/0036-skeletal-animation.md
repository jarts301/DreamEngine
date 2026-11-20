# 0036. Skeletal animation

- Status: Accepted
- Date: 2026-11-19

## Context

Models were static: `parse_glb` baked every node's transform into the vertices and ignored glTF's
skins, joint weights and animations. Characters, creatures and anything that moves by itself need
skeletal animation: a skeleton posed from keyframed clips, blended between clips, and meshes
skinned to it on the GPU.

The roadmap named ozz-animation (MIT) as the library. vcpkg's registry doesn't have it. The
graphics interface's only uniform block held 512 bytes, 8 matrices, and a character needs tens of
joints.

## Decision

### The library: ozz-animation through an overlay port

`cmake/ports/ozz-animation` builds ozz-animation 0.17.0's base, runtime and offline libraries, with
none of its samples, tools or importers, and installs a CMake package with the targets `ozz::base`,
`ozz::animation` and `ozz::animation_offline`, which ozz itself doesn't provide. Its own setting of
warnings as errors is removed, as newer compilers than its release trip over it. The presets add
`cmake/ports` to vcpkg's overlay ports beside the overlay triplets. ozz uses no exceptions or RTTI.

### An `animation` interface

`animation::AnimationSystem` builds a skeleton from joint parents and rest transforms, clips from
keyframes per joint, and players, one per animated object, holding the backend's caches. Each
frame, `pose` samples layers (a clip, a time and a weight), blends them, and writes each joint's
transform in the skeleton's space.

- **`ozz`** builds ozz's skeleton and compressed animations with its offline builders as a model
  loads, and poses with its sampling, blending and local-to-model jobs, four joints at a time.
  ozz orders joints depth first; the module keeps a map from the interface's order to its own.
  Channels a clip doesn't animate keep the joint's rest value.
- **`null`** gives the rest pose, so a project without an animation library still draws its
  characters, standing still.

Keys are interpolated linearly, as ozz does. The loader turns glTF's step keys into pairs of keys
a moment apart, and samples cubic-spline curves eight times between keys.

### Loading skins

`parse_glb` reads a skeleton made of every node a skin uses as a joint and every node above one,
so an armature's own transform and animation pose the model as glTF says. Each skin keeps its
joints, as skeleton indices, and its inverse bind matrices. Skinned meshes stay in their own
space, ignoring their node's transform, as glTF requires. A mesh without a skin that hangs below a
joint, such as a sword in a hand, follows that joint wholly, through one more skin.

Every vertex gains four joint indices and four weights, as bytes, the weights adding up to exactly
255: `MeshVertex` is 56 bytes. A skin has at most 256 joints.

`create_model_asset` keeps the skeleton, skins and clips, each skin's matrices at rest, and for each
skinned part a box per joint around the vertices that joint moves.

### A per-draw uniform block

`CommandList::set_draw_uniforms` sets a second uniform block of up to 16 KB, the most every
OpenGL ES 3 and Vulkan device takes in one block, at binding 8, register b2, or OpenGL uniform
block binding 8, on all six backends:

- Vulkan: a second dynamic uniform buffer in the per-frame ring, which grows to 4 MB.
- Direct3D 11: a third dynamic constant buffer.
- Direct3D 12: a root constant buffer view in the per-frame constant chunks.
- OpenGL and OpenGL ES: a third block in the uniform ring, which grows to 4 MB.

### Drawing skinned parts

`MeshDraw::joint_matrices` holds a skinned part's skinning matrices. The forward renderer sends
them as 3x4 rows, 256 joints in 12 KB, and has skinned versions of its lit, cut-out and shadow
shaders, compiled from the same files with `DREAM_SKINNED` defined, which include
`skinning.slang`. Joints and weights are read as normalized bytes, after the instance's inputs; a
skinned lit draw has 16 vertex inputs, the most OpenGL ES guarantees. Each skinned part is its own
draw, since each has its own pose. The runtime gives each skinned part a box made from its joints'
boxes as posed, so culling stays correct while it moves.

### Playing animation

`runtime::Animation`, like `Audio` and `Physics3D`, creates the configured module and adds the
scene component `animator`. `Animation::update` moves every `Animator`'s clip on, loops or holds it,
blends the clip fading out after `play`, and stores each skin's matrices in the `Animator` for the
renderer. Models without an `Animator` draw at rest. The module's skeletons and clips are built
once per model, and players are released when their entity stops animating.

The build now also recompiles a module's shaders when any Slang file beside them changes, since
`skinning.slang` is only included.

## Consequences

- Skinned glTF models from Blender and similar tools load and animate. The render demo has a
  tentacle with two clips, "sway" and "curl", that a key or gamepad button fades between, and its
  checks hold the animation at half a second so pictures stay the same.
- Every model's vertices grow by 8 bytes.
- Clips are built as models load; cooking them at build time, as levels of detail should be, is
  later work.
- Each skinned part costs a draw per pass, and its joints are sent again for each: many characters
  would want them uploaded once per frame.
- Root motion, animation events, inverse kinematics, blend trees, additive layers, morph targets
  and ragdolls are later steps. The interface's layers and players leave room for them.
