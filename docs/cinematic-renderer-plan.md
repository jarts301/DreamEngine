# DreamEngine: plan for a cinematic renderer

Goal: make DreamEngine able to render animated 3D films and AAA-style cinematics, with the
picture quality of feature animation: physically based light transport, film camera effects,
production colour management, and frames written as EXR and video.

This plan is a chain of 37 tasks. Each task uses the result of the one before, and each one
should take an experienced engineer **15 to 40 minutes**. That is only possible because every task
is cut narrowly: each one has an **Out of scope** line. If a task runs long, stop at that line,
write down what is left, and make it a task of its own. Don't let the task grow.

It reflects the repository on 2026-11-24 (after Editor v1, `e4e68b9`).

---

## How to read a task

Every task has the same parts:

| Part | Meaning |
|---|---|
| **Input** | What the earlier tasks must have produced |
| **Problem** | Why this is hard, and what usually goes wrong |
| **Phases** | Analysis → Decision → Build → Diagnose → Verify. The *Diagnose* phase names the failure you are expected to hit, so you know what to look for |
| **Decision** | The architectural choice you must make. The options and the criteria are given; the choice is yours, and goes in a short ADR |
| **CLion** | The CLion features the task relies on |
| **Other tools** | Anything outside CLion |
| **Pass check** | A measurable result that says the task is done |
| **Output** | What the next task receives |

### Rules for every task

1. **Modularity** ([modularity rule](roadmap.md#1-the-rule-every-plan-follows-everything-is-a-module)):
   each new capability is an interface in `interfaces/` with a real backend and a `null` backend,
   selected in `dream.toml`. The film renderer is a new **renderer module, `cinematic`**, beside
   `forward`; it does not replace it.
2. **Licenses** ([ADR 0022](adr/0022-licenses-and-frozen-libraries.md)): MIT, Apache 2.0, BSD,
   zlib or public domain only. This rules out OpenVDB/NanoVDB (MPL 2.0) and linking FFmpeg (LGPL),
   so the plan works around both (tasks 28 and 35).
3. **Platforms:** the offline path tracer is PC-only (Vulkan and Direct3D 12 on the RTX 5080). The
   real-time preview features (motion vectors, TAA, ACES output, depth of field) must keep running
   on the phones, since Android never falls behind for features a game can use.
4. **One task, one branch, one ADR** when a decision is made. ADR numbers continue from 0047.
5. **Every pass check is automated** where possible: a CTest test, a screenshot check, or a script
   whose output is committed under `docs/reports/cinematic/`.

### Tools used across the plan

| Tool | Used for | License |
|---|---|---|
| CLion (CMake presets `local-windows-*`, MSVC 14.38) | Everything | — |
| RenderDoc | Frame captures on every backend | MIT |
| NVIDIA Nsight Graphics | Ray tracing, shader profiling, GPU traces on the RTX 5080 | Free tool |
| Vulkan validation layers (with synchronization validation) | Barrier and hazard checks | Apache 2.0 |
| PIX on Windows | Direct3D 12 captures and timing | Free tool |
| NVIDIA FLIP (`flip` CLI and Python) | Perceptual image difference, the judge of most checks | BSD-3 |
| tev | Viewing and comparing HDR/EXR images pixel by pixel | BSD-3 |
| OpenImageIO `oiiotool` / `idiff` | EXR inspection, OCIO reference conversions | Apache 2.0 |
| Mitsuba 3 | Reference images for light transport | BSD-3 |
| Blender | Test assets, reference animation curves, Cryptomatte checks | Tool only |
| FFmpeg / ffprobe (as separate programs) | Encoding and checking video | Run as a process, never linked |
| Python 3 + NumPy | Analysis scripts, run from CLion's Python support | — |

---

## Milestone A — A judge, determinism, and real HDR output

Nothing in a film renderer can be judged by eye. These three tasks build the measuring
instruments the other 34 depend on.

### Task 01 — A perceptual image judge for every later check

- **Input:** the screenshot checks in `runtime/src/screenshot.cpp` (8-bit tolerance and a share
  of differing pixels).
- **Problem:** the 8-bit "pixels that differ by more than N" rule cannot judge noise, HDR values,
  or soft differences like a changed blur. Path-traced images are never bit-identical. A wrong
  threshold either passes broken images or fails good ones on every backend.
- **Phases:**
  1. *Analysis:* render every existing GPU reference on all five PC backends and collect the
     differences between backends. This is the noise floor today.
  2. *Decision:* choose the metric(s) and their thresholds.
  3. *Build:* add an `[screenshot] metric = "flip"` option with mean and 99th-percentile limits,
     and a float comparison mode (relative error with an epsilon) for EXR.
  4. *Diagnose:* the first run shows that sRGB-encoded and linear images give very different FLIP
     scores for the same change. Find where the colour encoding is lost in the comparison.
  5. *Verify:* see the pass check.
- **Decision:** FLIP (perceptual, made for rendering) vs SSIM vs PSNR/RMSE. Criteria: does the
  metric see the errors renderers make (fireflies, aliasing, colour shifts), can it compare HDR,
  is it cheap enough to run in CTest? Thresholds come from the measured noise floor, not by guess.
- **CLion:** CTest run configurations with arguments; debugger *View as Array* on the pixel buffer
  pointer; a conditional breakpoint that stops only on pixels whose error is above the limit;
  *Evaluate Expression* to sample pixel values while stopped.
- **Other tools:** FLIP, tev, Python + NumPy for the noise-floor histogram.
- **Pass check:** (a) an image against itself scores 0; (b) the existing references still pass on
  all backends; (c) a deliberately changed image (a 1-pixel shift, a 2% exposure change) fails;
  (d) `docs/reports/cinematic/noise-floor.md` lists the per-backend floor and the chosen limits.
- **Output:** a judge used by every later pass check, and the thresholds.

### Task 02 — Deterministic offline time

- **Input:** the judge from Task 01; `core/clock.h`; the job system.
- **Problem:** a film frame must be exactly reproducible: rendering frame 120 alone must give the
  same picture as frame 120 in a full run. Wall-clock time leaks in through particles, animation,
  physics, bloom and the frame limiter. The job system can also add floating-point values in a
  different order on each run.
- **Phases:**
  1. *Analysis:* find every place that reads real time or keeps state between frames.
  2. *Decision:* how seeking to frame N works.
  3. *Build:* `[app] time_mode = "offline"`, `fps = 24` (also 23.976 as 24000/1001), where time
     comes from the frame number as a rational, never from a sum of float steps.
  4. *Diagnose:* two runs differ in a few pixels of the campfire. Find the cause: the particle
     random seed, or a parallel reduction whose order depends on the thread that finishes first.
  5. *Verify:* see the pass check.
- **Decision:** seeking by replaying the simulation from frame 0, by saving state snapshots at
  intervals, or by requiring that everything evaluated in offline mode is a pure function of time
  (with physics baked to a cache first). Criteria: render-farm style frame splitting, cost, and
  which systems can never be pure (physics, particles).
- **CLion:** *Call Hierarchy* (Ctrl+Alt+H) and *Find Usages* on the clock functions; *Data Flow
  Analysis* to see where the time value travels; a **watchpoint** on the particle RNG state; the
  *Parallel Stacks* view to see which job threads run the reduction.
- **Other tools:** none.
- **Pass check:** a CTest that renders frames 0–47 twice and gets identical hashes, and renders
  frame 24 alone and gets the same hash as frame 24 of the full run. Done on Vulkan and D3D12.
- **Output:** an offline clock that later tasks use for subframes, sequences and batch rendering.

### Task 03 — Reading back HDR frames and writing EXR

- **Input:** the offline clock (Task 02); the HDR target from Phase 12.
- **Problem:** films are delivered as linear float images, not tone-mapped 8-bit PNGs. Reading a
  GPU image back each frame stalls the GPU unless it is pipelined, and R11G11B10 loses precision
  and cannot hold negative values.
- **Phases:**
  1. *Analysis:* measure what a naive read-back after each frame costs in the timeline profiler.
  2. *Decision:* the read-back design and the EXR library.
  3. *Build:* an `image_writer` interface (`exr`, `png`, `null`) and a read-back ring of N staging
     buffers with fences; in offline mode the renderer forces an Rgba16Float or Rgba32Float target.
  4. *Diagnose:* the EXR shows the right picture but flipped, or with rows shifted. Find the
     row-pitch alignment (256 bytes on D3D12) or the GL origin difference.
  5. *Verify:* see the pass check.
- **Decision:** OpenEXR (BSD-3, full features, multi-part for Task 33) vs tinyexr (BSD-3, one
  file, fewer features). Half vs float per channel. Ring depth (2 or 3) vs added latency.
- **CLion:** *Memory View* on the mapped staging buffer to see the real row pitch; CMake presets to
  add the vcpkg dependency per platform; the timeline profiler's markers in the debugger's output.
- **Other tools:** tev, `oiiotool --info --stats`.
- **Pass check:** an emissive test patch of exactly 4.0 reads 4.0 ± half-float precision in tev;
  writing every frame adds no GPU idle gap in the timeline capture; Vulkan and D3D12 EXRs compare
  equal under the Task 01 float metric.
- **Output:** linear EXR frames, which every later visual check uses as its source.

---

## Milestone B — Compute and a render graph

The renderer has no compute shaders yet (ADR 0037 notes this). A film renderer is mostly compute.

### Task 04 — Compute in the RHI: design and Vulkan

- **Input:** `interfaces/rhi/include/dream/rhi/rhi.h`; EXR output for checking.
- **Problem:** adding compute means storage images and buffers, dispatch, and, most of all, a
  barrier model. Today the RHI hides barriers inside render passes. Compute writes followed by
  reads need explicit synchronization, and the choice made here shapes Tasks 06–08.
- **Phases:**
  1. *Analysis:* list how each of the six backends expresses compute and barriers (Vulkan
     pipeline barriers, D3D12 resource states, D3D11 implicit, GL `glMemoryBarrier`).
  2. *Decision:* the barrier model.
  3. *Build:* `create_compute_pipeline`, `bind_storage_texture/buffer`, `dispatch`, resource
     states, on Vulkan only; `Device::supports_compute()`.
  4. *Diagnose:* synchronization validation reports a write-after-write or read-after-write hazard
     between two dispatches. Find the missing or wrong stage/access mask.
  5. *Verify:* see the pass check.
- **Decision:** (a) explicit barriers in the RHI API, (b) automatic tracking of each resource's
  state inside the RHI, (c) the RHI stays stateless and a render graph (Task 06) computes all
  barriers. Criteria: correctness, cost per frame, how well it maps to D3D12 and to the GL
  backends, and how much of it Task 06 would duplicate.
- **CLion:** *Change Signature* and *Implement Methods* across the six device classes; *Type
  Hierarchy* of `CommandList`; a logging breakpoint (no suspend) that prints each barrier.
- **Other tools:** Vulkan validation with synchronization validation; RenderDoc.
- **Pass check:** a compute shader writes a known gradient that matches its reference exactly;
  sync validation reports nothing; the `null` RHI and the other backends report
  `supports_compute() == false` and the forward renderer still runs on them.
- **Output:** compute on Vulkan, with a barrier model Task 05 must follow.

### Task 05 — Compute on Direct3D 12, Direct3D 11, OpenGL and OpenGL ES

- **Input:** the compute API and barrier model from Task 04.
- **Problem:** the same Slang compute shader must reach DXIL/DXBC and GLSL. SPIRV-Cross maps
  storage images to image units differently from textures, GL ES 3.1 limits image formats, and
  Mali/Adreno/Xclipse have small shared-memory and workgroup limits.
- **Phases:**
  1. *Analysis:* compare the compute limits of each backend and the two phones.
  2. *Decision:* the portable subset.
  3. *Build:* the four backends; extend `cmake/DreamCrossCompileShader.cmake` for compute stages.
  4. *Diagnose:* on GL ES the gradient is right on PC but black on the phone. Find the format
     qualifier (`rgba16f` vs `rgba8`) or a missing `memoryBarrierImage`.
  5. *Verify:* see the pass check.
- **Decision:** the guaranteed minimum (workgroup size, shared memory, storage formats) every
  module may assume, vs per-device queries that modules must handle.
- **CLion:** the **CMake debugger** (breakpoints inside `DreamCrossCompileShader.cmake`) to see
  the exact SPIRV-Cross command; *Compare Files* on generated GLSL between two runs.
- **Other tools:** RenderDoc on GL and the phone; PIX for D3D12; Android GPU Inspector.
- **Pass check:** the Task 04 compute test matches its reference on all five PC backends and on
  the A57 and A52s; D3D12 debug layer and GL debug output are clean.
- **Output:** compute everywhere, with documented limits.

### Task 06 — A render graph: the core

- **Input:** compute with a barrier model (Tasks 04–05).
- **Problem:** the film renderer will have 30+ passes (G-buffer, path tracing, denoising, AOVs,
  accumulation). Writing their barriers and texture lifetimes by hand does not scale. A graph
  must order passes, cull unused ones, and produce barriers, without per-frame allocations (ADR
  0041 brought those to near zero).
- **Phases:**
  1. *Analysis:* write down the current forward frame as a list of passes with reads and writes.
  2. *Decision:* graph shape and lifetime.
  3. *Build:* `RenderGraph` with `add_pass`, resource handles, `compile()` (topological sort,
     culling of passes whose outputs nobody reads, barrier list), `execute()`. Run it on the null
     RHI only.
  4. *Diagnose:* a pass that writes the final image is culled because it is reached only through
     an "imported" resource. Fix how outputs are marked.
  5. *Verify:* see the pass check.
- **Decision:** where the graph lives. Renderer-internal code shared by `forward` and
  `cinematic` (a library), or a new interface? Rebuilt each frame vs compiled once and cached
  until the pass list changes. Criteria: the modularity rule, cost per frame, hot reload.
- **CLion:** Google Test/Catch2 integration with *Rerun Failed Tests*; *Extract Function* and
  *Extract Interface*-style refactors; ASan with the MSVC toolchain on the test target.
- **Other tools:** Graphviz to dump the compiled graph (`.dot`) for a visual check.
- **Pass check:** unit tests: a culled pass, a diamond dependency, a resource read before it is
  written (error), and a golden barrier list; zero heap allocations per `compile()` after warm-up
  (use the engine's heap counting).
- **Output:** a tested render graph not yet used for real drawing.

### Task 07 — Move the forward renderer onto the graph

- **Input:** the render graph (Task 06); the forward renderer's passes.
- **Problem:** the move must change no pixel and cost no time. Hidden ordering assumptions
  (bloom's down/up chain, lines and UI after tone mapping, `update_texture` only between passes)
  will break.
- **Phases:**
  1. *Analysis:* list the implicit ordering rules in the forward module.
  2. *Decision:* how the "between passes only" upload rule is expressed in the graph.
  3. *Build:* rewrite the frame as graph passes; keep the old path behind a setting for one task.
  4. *Diagnose:* one backend shows a one-frame-late shadow. Find the pass that read last frame's
     shadow atlas because of a wrong edge.
  5. *Verify:* see the pass check.
- **Decision:** uploads as their own graph passes vs a pre-frame upload phase outside the graph.
- **CLion:** *Local History* and *Compare with Branch* to diff behaviour against `main`; dependent
  breakpoints (enable a breakpoint in the shadow pass only after the bloom pass was hit).
- **Other tools:** RenderDoc (compare event lists before and after); the timeline profiler.
- **Pass check:** every existing GPU screenshot check passes under the Task 01 limits on five PC
  backends and both phones; frame time within ±3% of `main` on the RTX 5080 and the A57.
- **Output:** the forward renderer on the graph; the old path removed.

### Task 08 — Transient memory aliasing

- **Input:** the forward renderer on the graph (Task 07).
- **Problem:** a film frame at 4K with AOVs needs gigabytes of intermediate textures. Textures
  whose lifetimes don't overlap can share memory, but an aliased texture starts with garbage and
  needs the right initial layout or a discard, and a wrong overlap corrupts the image only
  sometimes.
- **Phases:**
  1. *Analysis:* print each transient texture's first and last use and its size.
  2. *Decision:* the packing algorithm and where aliasing is supported.
  3. *Build:* lifetime intervals, placement into heaps (Vulkan memory aliasing, D3D12 placed
     resources); other backends keep separate textures.
  4. *Diagnose:* bloom flickers in one frame out of fifty. Find the missing aliasing barrier or
     the texture used before its "discard/clear" on the first use.
  5. *Verify:* see the pass check.
- **Decision:** greedy first-fit by size vs interval-graph colouring; one heap per frame vs a
  persistent pool. Criteria: memory saved, compile time, determinism (Task 02).
- **CLion:** *Memory View* of the heap offsets; a conditional breakpoint when two live intervals
  share an offset range.
- **Other tools:** validation layers (memory aliasing checks); PIX memory view; RenderDoc.
- **Pass check:** transient GPU memory at 4K drops by at least 30% (memory budget counters);
  1000 frames of the render demo show no flicker (Task 01 judge on every 10th frame); validation
  clean.
- **Output:** a graph that can hold film-sized frames.

---

## Milestone C — Colour science and the camera

### Task 09 — The working colour space

- **Input:** EXR output (Task 03); PBR materials (ADR 0033); environment lighting (ADR 0040).
- **Problem:** rendering in linear Rec.709 clips saturated colours and gives wrong results for
  coloured light bouncing between coloured surfaces. Films render in a wide gamut (ACEScg).
  Changing it touches textures, light colours, the environment HDR and every reference image, and
  R11G11B10 has no sign bit, so out-of-gamut values become 0.
- **Phases:**
  1. *Analysis:* list every place colour enters the renderer (textures, material factors, light
     colours, environment, particles, UI).
  2. *Decision:* the working space and where conversion happens.
  3. *Build:* a `[renderer] working_space` setting; input conversion at cook time for textures and
     at load time for factors and lights.
  4. *Diagnose:* a saturated cyan light turns black in places with R11G11B10. Explain why
     (negative components after conversion) and decide how the auto HDR format reacts.
  5. *Verify:* see the pass check.
- **Decision:** linear Rec.709 vs ACEScg (AP1) vs linear Rec.2020. Convert textures at cook time
  (cheaper, needs a cook version bump) vs in the shader (flexible). What R11G11B10 means for the
  wide-gamut mode on phones.
- **CLion:** *Find Usages* of the colour types; *Structure* view of the cooker; CMake presets to
  rebuild the cooked assets.
- **Other tools:** `oiiotool --colorconvert`; tev; a ColorChecker texture with published values.
- **Pass check:** a rendered ColorChecker under a known white light gives each patch within
  ΔE2000 < 1 of its published value; the existing references still pass in Rec.709 mode.
- **Output:** scene-linear rendering in a chosen wide gamut.

### Task 10 — Colour management as a module, and the display transform

- **Input:** the working colour space (Task 09).
- **Problem:** films are viewed through an output transform (ACES 2.0, AgX, studio LUTs), defined
  in OpenColorIO configs. Running OCIO's processing in a shader per pixel is not possible on every
  backend; it must be baked into a 3D LUT, and a LUT on linear HDR values needs a shaper (a log
  curve) or it bands.
- **Phases:**
  1. *Analysis:* read the ACES studio config and pick the views needed (sRGB display, Rec.1886,
     P3-D65).
  2. *Decision:* LUT size and shaper.
  3. *Build:* a `color_management` interface: `ocio` (bakes a LUT at load), `builtin` (the
     current PBR Neutral, so phones keep working without OCIO), `null`. The tone map pass samples
     the LUT.
  4. *Diagnose:* dark areas band and highlights shift hue. Find the shaper range or the LUT
     filtering (trilinear vs tetrahedral).
  5. *Verify:* see the pass check.
- **Decision:** 33³ vs 65³ LUT; trilinear hardware filtering vs tetrahedral in the shader; which
  view is the default.
- **CLion:** the module template (`dream new`-style module layout); vcpkg manifest features per
  platform in CMake presets; *Inline watches* on LUT coordinates.
- **Other tools:** `oiiotool --ociodisplay` as the reference; OCIO configs; tev.
- **Pass check:** for a test EXR with values from 0.001 to 100, the engine's output differs from
  `oiiotool --ociodisplay` by at most 1/255 (max) and 0.2/255 (mean); phones run with `builtin`
  and match their old references.
- **Output:** a film-grade display transform, chosen per project.

### Task 11 — A physical camera and light units

- **Input:** colour management (Task 10).
- **Problem:** cinematographers think in focal length, sensor size, f-stop, shutter angle and ISO,
  and lights in lux, lumens and nits. The renderer uses a field of view and an exposure number.
  Without physical units, a sun and a candle can't share a shot, and depth of field (Task 17) and
  motion blur (Task 16) have no correct inputs.
- **Phases:**
  1. *Analysis:* list what the camera and lights store today.
  2. *Decision:* the units and the exposure equation.
  3. *Build:* a camera component with sensor size, focal length, focus distance, f-stop, shutter
     angle, ISO and gate fit; EV100 exposure; lights in physical units.
  4. *Diagnose:* the sunny-16 scene is about 0.7 stops off. Find the missing lens/vignetting
     constant (q ≈ 0.65) or the 12.5 vs 100 calibration constant.
  5. *Verify:* see the pass check.
- **Decision:** EV100 with the saturation-based or the standard-output-sensitivity formula;
  horizontal vs vertical vs fill gate fit; whether old scenes keep the simple camera.
- **CLion:** *Change Signature* across the camera API; unit tests in the test runner; the editor's
  field descriptions (`core/fields.h`) so the editor shows the new fields.
- **Other tools:** Blender (compare FOV from the same sensor and focal length).
- **Pass check:** unit tests: a sunlit (100 000 lux) 18% grey card at f/16, 1/100 s, ISO 100 lands
  at scene value 0.18 ± 2%; a 35 mm lens on a 36×24 mm sensor gives Blender's horizontal FOV to 0.01°.
- **Output:** physical camera parameters for Tasks 12–17.

### Task 12 — Animation curves with production tangents

- **Input:** the camera component (Task 11); the offline clock (Task 02).
- **Problem:** film animation is keyframed curves with Bézier tangents (weighted, broken,
  auto-clamped), as in Blender and Maya. Getting tangent semantics slightly wrong makes cameras
  overshoot. Rotation curves in Euler angles flip at gimbal lock; quaternion curves don't follow
  the animator's intent.
- **Phases:**
  1. *Analysis:* export a few Blender F-curves with every tangent type and study their data.
  2. *Decision:* curve representation and rotation handling.
  3. *Build:* a curve type with key, in/out tangents and interpolation modes; fast evaluation by
     solving the Bézier in time (Newton with bisection fallback).
  4. *Diagnose:* one curve is off near a key with a very short tangent. Find the solver that
     failed to converge, or the weighted-tangent case.
  5. *Verify:* see the pass check.
- **Decision:** Euler curves (animator-friendly, gimbal problems) vs quaternions (stable,
  less editable) vs Euler with unwrap/filtering; evaluation cost vs baking to samples.
- **CLion:** unit tests with data-driven cases; *Evaluate Expression* to sample curves while
  stopped; a **Python run configuration** for the comparison script.
- **Other tools:** Blender and its Python API to sample curves at 240 Hz as reference data.
- **Pass check:** for 20 Blender curves covering every tangent mode, the engine matches Blender's
  samples to within 1e-4 everywhere; evaluation needs no allocation.
- **Output:** a curve library for the sequencer.

### Task 13 — The sequencer module: shots and camera cuts

- **Input:** curves (Task 12); the offline clock (Task 02); scenes in TOML (ADR 0046).
- **Problem:** a film is shots, each with a camera, a frame range and tracks that drive entities.
  A camera cut must happen exactly on a frame boundary; any system with history (TAA, motion
  vectors, accumulation) must know about the cut, or it smears one shot into the next.
- **Phases:**
  1. *Analysis:* what an animator needs: shot list, handles, camera per shot, property tracks.
  2. *Decision:* the data model and who owns evaluation.
  3. *Build:* a `sequencer` interface (`curves`, `null`); a sequence file format; evaluation each
     offline frame; a "camera cut" event the renderer receives.
  4. *Diagnose:* the first frame after a cut has the wrong camera for one subframe. Find the
     half-open vs closed interval mistake at the shot boundary.
  5. *Verify:* see the pass check.
- **Decision:** tracks that write ECS components directly vs tracks that produce values a system
  applies; a shot's frame numbers as local vs global time; how the editor (ADR 0046) shows it later.
- **CLion:** *Generate* for the component boilerplate; *Rename* across TOML and C++ with text
  occurrences; logging breakpoints that print frame and shot.
- **Other tools:** none.
- **Pass check:** a 3-shot test sequence renders: the right camera on every frame (checked by an
  embedded frame/shot number in a test AOV or the log), the cut event on exactly the first frame of
  each shot, and identical output when frames are rendered out of order.
- **Output:** sequences that drive cameras and entities, and a cut signal for temporal effects.

---

## Milestone D — Temporal effects

### Task 14 — Motion vectors

- **Input:** the sequencer with cut events (Task 13); skinning (ADR 0036).
- **Problem:** TAA, motion blur and denoising all need the exact screen motion of every pixel. For
  skinned meshes that means last frame's bone matrices; for instanced and LOD-switched meshes,
  last frame's transform per instance; and the camera jitter must be removed or vectors wobble.
- **Phases:**
  1. *Analysis:* where previous-frame data must be kept (transforms, bones, camera).
  2. *Decision:* the vector encoding and how history is stored.
  3. *Build:* a velocity target (RG16F) written in the opaque pass; previous transforms in the
     instance data; previous bone palette for skinned draws; reset on cut.
  4. *Diagnose:* the skinned tentacle has correct vectors except for a one-frame spike every time
     its LOD changes. Find the bone palette or instance index that doesn't match last frame's.
  5. *Verify:* see the pass check.
- **Decision:** previous world matrices per instance (more memory) vs previous model-view-
  projection (less flexible); velocity in pixels vs NDC; keeping history in the ECS vs in the renderer.
- **CLion:** *Natvis* visualizers for the engine's math types so matrices read clearly in the
  debugger; a watchpoint on the previous-palette buffer; *Parallel Stacks* when the animator runs
  on jobs.
- **Other tools:** RenderDoc (pixel history on the velocity target).
- **Pass check:** a synthetic pan test: a camera moving 3.5 px/frame gives measured vectors of
  3.5 ± 0.01 px; skinned and static objects show no spikes over 200 frames; vectors are zero on the
  first frame of each shot. Runs on the phones too.
- **Output:** a velocity buffer for Tasks 15–17 and 29.

### Task 15 — Temporal anti-aliasing for the preview

- **Input:** motion vectors (Task 14).
- **Problem:** the real-time preview (and games) need TAA. It is a set of trade-offs: ghosting
  vs flicker vs blur, and the right answer depends on the history rejection rule.
- **Phases:**
  1. *Analysis:* render a 16× supersampled reference of three test scenes (static detail, a moving
     object, thin geometry).
  2. *Decision:* the rejection method and the reconstruction filter.
  3. *Build:* jittered projection (Halton 2,3), reprojection, neighbourhood clamp or clip in
     YCoCg, a sharpening option; history reset on cut.
  4. *Diagnose:* a moving object leaves a ghost trail on a dark background. Decide whether the cause
     is the clamp box, the missing disocclusion test, or vectors at object edges (dilation).
  5. *Verify:* see the pass check.
- **Decision:** clamp vs clip (variance clipping); Catmull-Rom vs bilinear history sampling;
  where TAA sits relative to tone mapping and bloom.
- **CLion:** Slang editing with a TextMate bundle; hot reload of shaders while the demo runs;
  a run configuration that sets TAA settings through environment variables or a test TOML.
- **Other tools:** FLIP (Task 01); RenderDoc.
- **Pass check:** FLIP against the 16× reference better than no AA on all three scenes; temporal
  flicker (per-pixel variance over 60 frames of the static scene) below a limit; ghost length under
  2 px in the moving test. A57 cost under 1 ms.
- **Output:** a stable preview image; jitter infrastructure used again by Task 16.

### Task 16 — Offline accumulation: exact anti-aliasing and motion blur

- **Input:** the offline clock (Task 02), jitter (Task 15), the physical camera's shutter (Task 11).
- **Problem:** films don't use TAA. Each frame averages many subframes, each with a different
  pixel offset and a different time within the shutter interval. That gives true motion blur, but
  every animated thing (sequencer, skinning, particles) must be evaluated at subframe times, and
  the sample pattern decides how fast noise goes away.
- **Phases:**
  1. *Analysis:* which systems can be evaluated at a fractional time and which can't.
  2. *Decision:* the sample sequence and the pixel filter.
  3. *Build:* an accumulation pass in the graph (float32 sum and weight), N subframes per frame,
     shutter open/close from the shutter angle.
  4. *Diagnose:* the blur of a spinning fan is straight instead of curved. Find that transforms are
     linearly interpolated between frames instead of evaluated at the subframe time.
  5. *Verify:* see the pass check.
- **Decision:** random vs Halton vs Sobol (Owen-scrambled) vs R2 sequences; box vs
  Blackman-Harris vs Gaussian filter; centred vs opening shutter.
- **CLion:** a conditional breakpoint on `subframe == N-1`; *Evaluate Expression* on time values;
  the timeline profiler to measure per-subframe cost.
- **Other tools:** Python + NumPy to plot error vs N (log-log); tev.
- **Pass check:** the error against a 1024-subframe reference falls faster than 1/√N with the
  chosen sequence (slope measured and recorded); a rotating fan's blur arcs match an analytic
  reference image (FLIP below limit).
- **Output:** an accumulation framework the path tracer and depth of field reuse.

### Task 17 — Depth of field

- **Input:** accumulation (Task 16); camera f-stop and focus distance (Task 11).
- **Problem:** offline, depth of field comes from jittering the camera across a thin-lens aperture
  per subframe; it's exact but needs many samples. The preview needs a fast gather method that
  doesn't bleed sharp foreground into blurred background (the classic artefact).
- **Phases:**
  1. *Analysis:* derive circle-of-confusion size from the lens equation.
  2. *Decision:* the preview algorithm.
  3. *Build:* thin-lens offsets in accumulation (with an aperture shape for polygonal bokeh);
     a preview gather DoF pass.
  4. *Diagnose:* in the preview, sharp objects in focus get a halo of the blurred background.
     Find the missing near/far separation or the wrong CoC sign.
  5. *Verify:* see the pass check.
- **Decision:** scatter-as-gather (separate near and far) vs a sprite-based bokeh vs a simple
  two-pass blur, given the phones' budget; uniform disk vs blade-shaped aperture sampling.
- **CLion:** data-driven unit tests for the CoC equation; shader hot reload.
- **Other tools:** Blender Cycles (same lens settings) for a reference image.
- **Pass check:** an out-of-focus point light's disc diameter matches the CoC formula within 1 px
  in the offline path; the offline image matches Cycles under the Task 01 limit; the preview has no
  halo in the near/far test scene.
- **Output:** the full camera model, for the cinematic renderer.

---

## Milestone E — The `cinematic` renderer and its materials

### Task 18 — The `cinematic` renderer module: visibility buffer

- **Input:** render graph (Tasks 06–08), compute (Tasks 04–05), motion vectors (Task 14).
- **Problem:** the film renderer needs per-pixel data (position, normal, material, IDs) for ray
  tracing, AOVs and denoising. A fat G-buffer costs bandwidth; a visibility buffer (triangle +
  instance ID) shades in compute but needs bindless-style access to all geometry and materials.
- **Phases:**
  1. *Analysis:* the per-pixel data later tasks will need (Tasks 25, 29, 33).
  2. *Decision:* visibility buffer vs G-buffer.
  3. *Build:* a new renderer module `cinematic` (with `dream_add_module`), sharing asset loading
     with `forward`; the primary visibility pass and a compute shading pass using the same BRDF.
  4. *Diagnose:* shading derivatives are wrong in compute (no screen-space derivatives), so
     textures alias. Implement analytic derivatives from barycentrics.
  5. *Verify:* see the pass check.
- **Decision:** visibility buffer vs G-buffer; how all geometry and materials become reachable
  from one shader (descriptor indexing vs big buffers); what `cinematic` reuses from `forward`
  without copying code.
- **CLion:** *Extract* refactorings to move shared BRDF code into a common Slang module; *Find in
  Files* with scope set to `modules/renderer`; a new CMake target via the module template.
- **Other tools:** RenderDoc; Nsight Graphics.
- **Pass check:** with `renderer = ["cinematic"]`, the render demo matches the `forward` image
  under the Task 01 limits; `forward` remains the default for games; `cinematic` falls back to
  `null` cleanly on a device without the needed features.
- **Output:** the cinematic renderer's frame, ready for better materials and ray tracing.

### Task 19 — Energy-conserving GGX

- **Input:** the cinematic renderer's shading (Task 18).
- **Problem:** single-scattering GGX loses up to 60% of energy at high roughness, so rough metals
  look dark. The fix (Kulla-Conty multiple-scattering compensation) needs precomputed tables, and a
  mistake gains energy, which is worse.
- **Phases:**
  1. *Analysis:* build the white furnace test: a sphere lit by a uniform white environment.
  2. *Decision:* table resolution and format, and whether the compensation also applies to `forward`.
  3. *Build:* a compute pass that integrates the directional albedo tables at start (or at cook
     time); the compensation term in the BRDF.
  4. *Diagnose:* the furnace sphere is brighter than 1.0 at grazing angles. Find the table's
     parameterization (cos θ vs θ) or the missing Fresnel average.
  5. *Verify:* see the pass check.
- **Decision:** tables computed at start vs cooked into assets; 32² vs 64²; sharing with `forward`.
- **CLion:** unit test of the CPU reference integration against the GPU table; *View as Array* on
  the read-back table.
- **Other tools:** Python + NumPy for the reference integral; tev.
- **Pass check:** the furnace sphere is 1.0 ± 1% everywhere for roughness 0–1, metallic and
  dielectric white; the old (uncompensated) result is reported for comparison.
- **Output:** a BRDF that conserves energy, the base of layered materials.

### Task 20 — Layered materials: coat, sheen, anisotropy

- **Input:** energy-conserving GGX (Task 19); the material system (ADR 0033).
- **Problem:** film materials (car paint, fabric, brushed metal) need layers. Each layer must
  attenuate the ones below it by energy, and the material data must grow without blowing up the
  per-instance size that ADR 0033 kept small.
- **Phases:**
  1. *Analysis:* map glTF extensions (`KHR_materials_clearcoat`, `sheen`, `anisotropy`) to an
     OpenPBR-like layering.
  2. *Decision:* the material data layout.
  3. *Build:* the three lobes in the cinematic BSDF with albedo-scaling between layers; loading
     from glTF.
  4. *Diagnose:* the coated furnace sphere exceeds 1.0. Find the base layer not being attenuated by
     the coat's directional albedo.
  5. *Verify:* see the pass check.
- **Decision:** a fixed fat material struct vs a feature-bit-indexed variable layout in a buffer
  vs material graphs compiled to shaders; which lobes the `forward` renderer also supports.
- **CLion:** *Natvis* for the packed material struct; *Find Usages* of the old material fields;
  `static_assert` sizes checked by the compiler.
- **Other tools:** Mitsuba 3 (reference spheres with the same parameters); the Khronos glTF sample
  models for clearcoat/sheen/anisotropy.
- **Pass check:** coated, sheen and anisotropic furnace spheres stay ≤ 1.0 + 1%; three reference
  spheres match Mitsuba under the Task 01 limit (environment lighting only).
- **Output:** a film material model.

### Task 21 — Subsurface scattering for skin

- **Input:** the material model (Task 20); the visibility buffer (Task 18).
- **Problem:** skin without subsurface scattering looks like plastic. Screen-space diffusion is
  cheap but wrong at thin features (ears); path-traced random walk is right but slow and needs Task
  25. A decision made now changes the path tracer later.
- **Phases:**
  1. *Analysis:* compare Burley's normalized diffusion profile and Jimenez separable SSS.
  2. *Decision:* which method for preview and which for final frames.
  3. *Build:* the preview method in the cinematic renderer, driven by mean free path per colour
     channel in the material.
  4. *Diagnose:* skin colour bleeds across the silhouette onto the background. Find the missing
     depth/material-aware weight in the blur.
  5. *Verify:* see the pass check.
- **Decision:** separable screen-space vs Burley screen-space vs leaving SSS to the path tracer
  (random walk) only; parameters in mean free path (artist friendly) vs scattering coefficients.
- **CLion:** shader hot reload; a profiling session in the timeline to measure the pass.
- **Other tools:** Mitsuba 3 (a slab and a head-like shape with the same medium) as reference.
- **Pass check:** the measured light falloff across a lit edge of a slab matches Mitsuba's
  profile within 10%; no bleeding in the silhouette test; cost at 1080p recorded.
- **Output:** skin for the preview, and the parameters the path tracer will use.

### Task 22 — Hair shading on cards

- **Input:** the material model (Task 20); accumulation (Task 16) for transparency.
- **Problem:** hair is thousands of thin, partly transparent cards. It needs its own BSDF
  (Marschner R/TT/TRT lobes, or Chiang's) and order-independent transparency; sorting thousands of
  cards per frame fails, and alpha blending flickers.
- **Phases:**
  1. *Analysis:* why the current alpha modes (ADR 0034) fail for hair.
  2. *Decision:* the transparency method.
  3. *Build:* a hair BSDF (energy-checked) and stochastic transparency that the accumulation
     resolves offline and TAA resolves in preview.
  4. *Diagnose:* the hair furnace test is too dark. Find the azimuthal scattering term not
     integrating to 1.
  5. *Verify:* see the pass check.
- **Decision:** Marschner vs Chiang (energy conserving, used in production path tracers);
  stochastic transparency vs weighted blended OIT vs alpha-to-coverage.
- **CLion:** unit test of the BSDF's numerical integral on the CPU; *Run to Cursor* for single-pixel
  shader debugging done in the CPU port of the BSDF.
- **Other tools:** a hair card asset from Blender; RenderDoc.
- **Pass check:** the hair BSDF integrates to 1.0 ± 2% (white furnace, CPU test); the hair test
  scene converges with accumulation to a stable image (no flicker across 24 frames).
- **Output:** a complete set of surface materials for the path tracer.

---

## Milestone F — Ray tracing and path tracing

### Task 23 — Ray tracing in the RHI

- **Input:** compute (Tasks 04–05); the cinematic renderer (Task 18).
- **Problem:** path tracing needs acceleration structures (BLAS per mesh, TLAS per frame) and ray
  queries. Vulkan (`VK_KHR_acceleration_structure`, `ray_query`) and DXR differ in build inputs,
  scratch memory and compaction. Only two of the six backends can support it.
- **Phases:**
  1. *Analysis:* map the two APIs side by side.
  2. *Decision:* ray query in compute vs ray tracing pipelines with shader tables.
  3. *Build:* `create_blas/tlas`, build and compaction, `supports_ray_tracing()`, on Vulkan and
     D3D12.
  4. *Diagnose:* the TLAS build crashes the driver (device lost) on the second frame. Find the
     scratch buffer alignment or the instance buffer reused while the GPU still reads it.
  5. *Verify:* see the pass check.
- **Decision:** inline ray queries (simpler, one compute shader, good for the path tracer) vs
  pipelines with shader binding tables (flexible, harder to port); compaction always vs never.
- **CLion:** *Implement Methods* for the two backends; *Type Hierarchy*; device-lost breakpoints
  in the error path; ASan on the build code.
- **Other tools:** Nsight Graphics (acceleration structure viewer); PIX; validation layers.
- **Pass check:** ray-traced hard shadows in the render demo match the shadow-mapped image's
  shadow shapes (judge on a shadow-only AOV); validation and the D3D12 debug layer are clean; all
  other backends report no support and nothing breaks.
- **Output:** ray queries on PC.

### Task 24 — Acceleration structures for animated scenes

- **Input:** ray tracing (Task 23); skinning and morphs; subframe accumulation (Task 16).
- **Problem:** skinned characters change every subframe. Rebuilding each BLAS is accurate but
  slow; refitting is fast but degrades as the mesh deforms far from its build pose. Motion blur
  needs the geometry at each subframe time.
- **Phases:**
  1. *Analysis:* measure build vs refit time for the tentacle and a dense character.
  2. *Decision:* the rebuild/refit policy.
  3. *Build:* compute skinning into a vertex buffer the BLAS reads; refit with a rebuild trigger.
  4. *Diagnose:* after a few hundred frames of refit, tracing gets 3× slower. Explain the BVH
     quality loss and confirm with Nsight.
  5. *Verify:* see the pass check.
- **Decision:** refit always / rebuild every N frames / rebuild when bounding volume grows by X%;
  a BLAS per subframe vs a motion-blur-aware structure (not portable).
- **CLion:** the timeline profiler; conditional breakpoints on the rebuild trigger.
- **Other tools:** Nsight Graphics.
- **Pass check:** a 10-second animation keeps trace cost within 20% of a freshly built structure on
  every frame; shadows match a full-rebuild run under the Task 01 limit.
- **Output:** correct, fast acceleration structures for animated shots.

### Task 25 — The path tracer core

- **Input:** ray queries (Tasks 23–24); the materials (Tasks 19–22); accumulation (Task 16);
  colour management (Task 10).
- **Problem:** an unbiased path tracer that matches a reference. Most errors are silent: a missing
  cosine, a wrong PDF, an MIS weight off by a factor; the image looks plausible but is wrong.
- **Phases:**
  1. *Analysis:* set up the Cornell box and a furnace scene in both DreamEngine and Mitsuba.
  2. *Decision:* the sampler and the integrator's structure.
  3. *Build:* a progressive path tracer in compute: GGX VNDF sampling, environment importance
     sampling (2D CDF), multiple importance sampling with the sun and analytic lights, Russian
     roulette.
  4. *Diagnose:* the Cornell box is 8% darker than Mitsuba. Find the bug by testing each piece
     alone (diffuse only, then direct only, then one bounce).
  5. *Verify:* see the pass check.
- **Decision:** Sobol + Owen scrambling vs blue-noise vs random; a megakernel vs a wavefront split
  (keep a megakernel now, Task 36 revisits); the path depth and roulette rule.
- **CLion:** a CPU port of one path (the same Slang functions compiled for C++ or a small C++ copy)
  that can be stepped in the debugger for a chosen pixel; *Evaluate Expression* on PDFs; unit tests
  that check each sampling routine's PDF integrates to 1.
- **Other tools:** Mitsuba 3; FLIP; tev.
- **Pass check:** furnace test passes at every bounce depth; the Cornell box at 4096 spp matches
  Mitsuba with FLIP mean < 0.05; the PDF unit tests pass.
- **Output:** a reference-quality path tracer.

### Task 26 — Many lights

- **Input:** the path tracer (Task 25); the clustered lights (ADR 0037).
- **Problem:** cinematics have hundreds of practical lights. Picking one light at random gives
  extreme noise. Light trees and ReSTIR solve it differently; ReSTIR reuses samples across pixels
  and frames, which conflicts with frame independence (Task 02) unless restricted.
- **Phases:**
  1. *Analysis:* a 1000-light test scene; measure the noise of uniform light picking.
  2. *Decision:* the light sampling method.
  3. *Build:* the chosen method.
  4. *Diagnose:* the image converges, but to the wrong brightness. Find the light selection PDF
     not matching the probability used.
  5. *Verify:* see the pass check.
- **Decision:** a light BVH (stateless, frame-independent, unbiased) vs ReSTIR DI (lower noise,
  temporal state, bias if done naively) vs power-weighted picking; what happens on a camera cut.
- **CLion:** unit tests on the CPU light tree; *Memory View* on the GPU node layout.
- **Other tools:** Python + NumPy (variance maps).
- **Pass check:** at equal samples, per-pixel variance drops at least 5× vs uniform picking; the
  converged image matches the uniform-picking converged image (unbiased) under the Task 01 limit.
- **Output:** scenes with many lights at film quality.

### Task 27 — Numerical robustness: acne, fireflies, NaNs

- **Input:** the path tracer with many lights (Task 26).
- **Problem:** production frames must not contain one NaN pixel, black acne or fireflies.
  Each fix has a cost: ray offsets that are too big make light leak through thin walls; clamping
  fireflies removes energy (bias).
- **Phases:**
  1. *Analysis:* a stress scene with thin walls, very small and very bright lights, and glossy
     surfaces; count NaN and firefly pixels over 1000 frames.
  2. *Decision:* the robustness rules and their bias.
  3. *Build:* robust ray origin offset (scaled to the hit position's float precision), a NaN/Inf
     guard that reports the pixel and path, the chosen firefly treatment.
  4. *Diagnose:* a NaN appears in one frame out of 300. Use the guard's report to reproduce that
     exact path on the CPU port (Task 25) and fix the division.
  5. *Verify:* see the pass check.
- **Decision:** clamp path contributions (biased, simple) vs path regularization (roughen after
  glossy bounces) vs leave it unbiased and rely on the denoiser; offset by epsilon vs by
  floating-point-precision method.
- **CLion:** the CPU port in the debugger with the reported seed and pixel; *Set Execution Point*
  to rerun a function after a fix in *Evaluate Expression*; a hardware watchpoint on the first NaN.
- **Other tools:** tev (NaN highlighting); Python.
- **Pass check:** zero NaN/Inf pixels across 1000 frames of the stress scene; no light leak through
  the 1 mm wall; the bias from firefly handling is measured and stays under 1% of mean brightness.
- **Output:** a path tracer safe for long renders.

### Task 28 — Volumes: fog, god rays and smoke

- **Input:** the robust path tracer (Task 27); the particles' smoke (ADR 0038) as a source.
- **Problem:** volumes are how films get atmosphere. Heterogeneous media need free-flight sampling
  (delta tracking) that is unbiased only if the majorant really bounds the density. OpenVDB is
  MPL-licensed, so the engine needs its own grid format.
- **Phases:**
  1. *Analysis:* the media needed (homogeneous fog, a heterogeneous smoke grid) and how they're
     placed in scenes.
  2. *Decision:* the grid format and the tracking method.
  3. *Build:* a sparse brick grid (own format, cooked from a simple input); delta tracking for
     scattering, ratio tracking for shadow rays; a `volume` component.
  4. *Diagnose:* the smoke looks right but its shadows are too dark in dense bricks. Find the
     majorant per brick being smaller than the real maximum after trilinear filtering.
  5. *Verify:* see the pass check.
- **Decision:** a global majorant vs per-brick majorants; delta/ratio tracking vs the
  null-scattering formulation with MIS; how a Blender smoke simulation gets exported to the format.
- **CLion:** unit tests comparing transmittance estimates against analytic Beer-Lambert; *Memory
  View* on the brick pool.
- **Other tools:** Blender (smoke export via a small Python script); Mitsuba 3 for a heterogeneous
  reference.
- **Pass check:** measured transmittance through a homogeneous slab equals Beer-Lambert within 1%;
  a heterogeneous volume matches Mitsuba under the Task 01 limit; god rays appear through a window
  in the test scene.
- **Output:** volumetric media in shots.

### Task 29 — The denoiser module

- **Input:** the path tracer (Tasks 25–28); albedo and normal data from the visibility buffer
  (Task 18); motion vectors (Task 14).
- **Problem:** final frames can't take 16 000 samples per pixel. A denoiser (Intel Open Image
  Denoise, Apache 2.0) works well per frame but each frame is denoised independently, so detail
  flickers between frames. Its quality depends on clean auxiliary images.
- **Phases:**
  1. *Analysis:* render a 24-frame shot at 64 spp and a 16k spp reference for frames 0, 12, 23.
  2. *Decision:* auxiliary images and temporal handling.
  3. *Build:* a `denoiser` interface (`oidn`, `null`); albedo and normal AOVs at the first
     non-specular hit; GPU buffer sharing with OIDN where possible, read-back otherwise.
  4. *Diagnose:* mirrors and glass come out blurred. Find that the albedo/normal AOV is taken at
     the mirror instead of at the first diffuse-like hit behind it.
  5. *Verify:* see the pass check.
- **Decision:** prefiltered vs noisy auxiliaries; denoise beauty only vs per-AOV (diffuse and
  specular separately, which compositors want); a temporal blend using motion vectors vs none.
- **CLion:** vcpkg features in presets for OIDN; CMake debugger if the OIDN device libraries are
  not staged; timeline markers around the denoise call.
- **Other tools:** FLIP; tev; Python for the flicker metric.
- **Pass check:** at 64 spp, the denoised frames score FLIP mean < 0.06 against the 16k spp
  references; flicker (frame-to-frame difference on static areas) under a measured limit; `null`
  leaves the image noisy but the pipeline runs.
- **Output:** final-quality frames in reasonable time.

---

## Milestone G — Character animation for film

### Task 30 — Morph targets for faces

- **Input:** skeletal animation (ADR 0036); compute (Tasks 04–05); motion vectors (Task 14).
- **Problem:** facial animation uses hundreds of blend shapes. Applying them on the GPU before
  skinning, with sparse deltas, without breaking normals, tangents, or last frame's positions (for
  motion vectors and BLAS refits), is the challenge.
- **Phases:**
  1. *Analysis:* glTF morph target data and how sparse it is for a face.
  2. *Decision:* the storage and evaluation order.
  3. *Build:* morph weights as animation channels (and sequencer tracks); a compute pass that
     applies active targets then skins; previous-frame positions kept.
  4. *Diagnose:* lighting on the face shows faceting during a smile. Find the normals not being
     morphed (or not renormalized), or tangents left from the rest pose.
  5. *Verify:* see the pass check.
- **Decision:** dense vs sparse delta storage; morph on the GPU vs on the CPU for small counts;
  normal deltas from the file vs recomputed.
- **CLion:** *Natvis* for the sparse delta structures; a conditional breakpoint on a chosen vertex
  index; *Parallel Stacks* if weights are evaluated in jobs.
- **Other tools:** Blender (export a face with shape keys and the reference positions at set
  weights).
- **Pass check:** for 10 weight combinations, vertex positions match Blender's evaluated mesh to
  1e-5; motion vectors on the face are smooth over a 100-frame animation; the BLAS refit (Task 24)
  works with morphs.
- **Output:** facial animation.

### Task 31 — Dual-quaternion skinning

- **Input:** skinning (ADR 0036, Task 30's order).
- **Problem:** linear blend skinning collapses joints that twist (the "candy wrapper") and lose
  volume at bends. Dual quaternions fix twisting but bulge at bends and can't handle scale.
- **Phases:**
  1. *Analysis:* build a twist test and a bend test; measure volume loss with LBS.
  2. *Decision:* the method, per character or per joint.
  3. *Build:* DQS in the skinning shader (and compute skinning from Task 24), selectable per mesh;
     handle quaternion sign (antipodality).
  4. *Diagnose:* one vertex flips across the mesh at 180° twist. Find the missing sign alignment
     to a pivot quaternion.
  5. *Verify:* see the pass check.
- **Decision:** LBS vs DQS vs a blend of both per vertex vs "optimized centres of rotation";
  what happens with joint scale.
- **CLion:** *Natvis* for dual quaternions; unit tests of the math in `core/math`.
- **Other tools:** Blender (the same rig with "Preserve Volume" on as a reference).
- **Pass check:** the twist test keeps ≥ 95% of the rest volume at 180° (LBS drops near 0 at the
  joint); positions match Blender's preserve-volume result to 1e-3 of the model size.
- **Output:** film-quality character deformation.

### Task 32 — Animation caches with Alembic

- **Input:** the offline clock (Task 02); subframe evaluation (Task 16); BLAS updates (Task 24).
- **Problem:** cloth, hair and effects come from other software as baked caches. Alembic (BSD)
  streams geometry per frame; meshes can change topology, and motion blur needs values between
  cached samples, which is only valid when topology doesn't change.
- **Phases:**
  1. *Analysis:* a cloth and a fluid cache from Blender, one with changing topology.
  2. *Decision:* streaming and interpolation.
  3. *Build:* a `geometry_cache` interface (`alembic`, `null`), streamed on jobs a few frames
     ahead; interpolation between samples; velocity from the cache when present.
  4. *Diagnose:* the fluid's motion blur explodes in some frames. Find interpolation across a
     topology change and switch to cached velocities there.
  5. *Verify:* see the pass check.
- **Decision:** read ahead on jobs vs memory-map; interpolate positions vs use stored velocities;
  how caches enter the asset pipeline and cooking.
- **CLion:** *Parallel Stacks* to check streaming jobs; a memory budget breakpoint; ASan on the
  reader.
- **Other tools:** Blender (export the caches); `abcls`/`abcecho` to inspect files.
- **Pass check:** cloth positions at subframe times match Blender's subframe export to 1e-4;
  streaming never stalls a frame (timeline); the topology-change cache renders without exploding
  blur.
- **Output:** caches from other tools inside shots.

---

## Milestone H — Production output

### Task 33 — AOVs, multi-layer EXR and Cryptomatte

- **Input:** the path tracer and denoiser (Tasks 25–29); EXR output (Task 03).
- **Problem:** compositors need separate passes (diffuse, specular, emission, volume, depth,
  normals, motion) and Cryptomatte ID mattes. Cryptomatte has a precise spec (MurmurHash3 of
  names, float-encoded IDs, ranked coverage, a manifest in metadata) that tools reject if any of it
  is off.
- **Phases:**
  1. *Analysis:* read the Cryptomatte spec and list the AOVs to write.
  2. *Decision:* how AOVs are declared and how coverage is accumulated.
  3. *Build:* AOV declarations in the graph; light-path expressions for diffuse/specular; ranked
     ID coverage accumulation; a multi-part EXR with metadata through `image_writer`.
  4. *Diagnose:* Blender's compositor reads the Cryptomatte but picks the wrong object. Find the
     hash-to-float conversion (exponent bits must be kept away from NaN/denormals) or the manifest
     name mismatch.
  5. *Verify:* see the pass check.
- **Decision:** fixed AOV list vs user-declared light-path expressions; how many Cryptomatte ranks;
  half vs float per AOV.
- **CLion:** unit tests against the spec's published hash examples; *Memory View* on the
  per-pixel rank buffer.
- **Other tools:** Blender's compositor or Natron with the Cryptomatte plugin; `oiiotool --info -v`.
- **Pass check:** the sum of the light AOVs equals the beauty within 0.1%; picking each object in
  Blender's Cryptomatte node isolates exactly that object (edge coverage sums to 1).
- **Output:** frames a compositor can use.

### Task 34 — The batch renderer: `dream render`

- **Input:** everything above: sequences, path tracer, AOVs.
- **Problem:** films are rendered unattended for hours. The renderer must run without a window,
  render frame ranges, survive and resume after a crash or device loss, and avoid the Windows GPU
  timeout (TDR) that resets the driver when one dispatch runs longer than ~2 s.
- **Phases:**
  1. *Analysis:* what can fail in a 10-hour render (TDR, device lost, disk full, a corrupt frame).
  2. *Decision:* the resume and TDR strategy.
  3. *Build:* a `dream render shot.toml --frames 1-240 --spp 512 --out ...` command; headless mode
     (no swapchain); tiled dispatches with a time budget; per-frame atomic writes (temp file then
     rename); a progress log and resume.
  4. *Diagnose:* a 4K frame at high spp triggers a device lost. Confirm it is TDR, then size the
     tiles from measured time per tile instead of a fixed size.
  5. *Verify:* see the pass check.
- **Decision:** tiles sized by time vs fixed; raising `TdrDelay` (needs admin, affects the user's
  machine) vs never relying on it; resume by "frame file exists and is valid" vs a job journal.
- **CLion:** run configurations with program arguments and environment; *Attach to Process* to a
  running render; the crash handler's minidump opened in the debugger.
- **Other tools:** Windows Event Viewer (TDR events); Task Manager GPU view.
- **Pass check:** kill the process at frame 50 of 100, run again, and the result is
  byte-identical to an uninterrupted run (Task 02); a 4K frame at 4096 spp completes without a
  device lost; frames are never left half-written.
- **Output:** reliable unattended rendering.

### Task 35 — Video encoding without linking FFmpeg

- **Input:** EXR frames from the batch renderer (Task 34); colour management (Task 10).
- **Problem:** the result must be a video with correct colours. FFmpeg is LGPL, so it cannot be
  linked (ADR 0022), but it can run as a separate program fed through a pipe. Most "the video looks
  washed out" problems come from mismatched colour tags (sRGB vs BT.709/BT.1886, full vs limited
  range), not from the renderer.
- **Phases:**
  1. *Analysis:* the delivery formats needed (ProRes/DNxHR for editing, H.264/H.265 10-bit for
     viewing) and their colour metadata.
  2. *Decision:* the encoder boundary.
  3. *Build:* a `video_encoder` interface (`ffmpeg_process`, `null`) that pipes frames already
     converted by the display transform; correct `color_primaries`, `color_trc`, `colorspace` and range flags.
  4. *Diagnose:* the video is darker than the EXR shown in tev. Decide whether the cause is the
     transfer curve (sRGB vs BT.1886), limited-range scaling, or the player, using measured values.
  5. *Verify:* see the pass check.
- **Decision:** pipe raw frames vs write an image sequence and run FFmpeg after; where the display
  transform is applied (engine vs FFmpeg filter); 8-bit vs 10-bit.
- **CLion:** step through the new process-launch code in the debugger; a logging breakpoint on pipe
  write sizes; run configuration with FFmpeg's path as an environment variable.
- **Other tools:** FFmpeg and ffprobe; a colour patch test chart; tev.
- **Pass check:** `ffprobe` shows the intended colour tags; decoding the video back gives each
  colour patch within 1 code value (10-bit) of the expected value; the `null` encoder leaves the
  EXRs as the deliverable.
- **Output:** a watchable, correctly tagged film.

### Task 36 — Making the path tracer fast

- **Input:** the complete path tracer (Tasks 25–33); the capstone-like test scene.
- **Problem:** render time decides whether a film can be made at all. A single "megakernel" path
  tracer suffers from divergence (different materials in one warp) and register pressure. The fix
  (wavefront: separate kernels per stage with queues) is a large structural change, and its benefit
  must be proven, not assumed.
- **Phases:**
  1. *Analysis:* an Nsight GPU trace: occupancy, warp divergence, registers, memory throughput.
  2. *Decision:* what to change.
  3. *Build:* the chosen change (material sorting, wavefront split, or register reductions).
  4. *Diagnose:* the faster version gives a slightly different image. Prove whether it's noise
     (different sample order) or a bug, using the converged comparison.
  5. *Verify:* see the pass check.
- **Decision:** megakernel with sorting vs wavefront vs shader execution reordering
  (vendor-specific; would it stay a module option?); the measure of success (samples/second at
  equal image error).
- **CLion:** the timeline profiler and CPU profiling of the host side; *Compare with Branch* to keep
  the old kernel as a baseline.
- **Other tools:** Nsight Graphics (GPU Trace, shader profiler); FLIP.
- **Pass check:** at least 1.5× more samples per second on the capstone scene at an equal
  converged image (FLIP of converged images below the Task 01 floor); results recorded in
  `docs/reports/cinematic/performance.md`.
- **Output:** a renderer fast enough for the final shot.

### Task 37 — The capstone: a 10-second cinematic

- **Input:** every task above.
- **Problem:** features that each pass alone often fail together: motion blur on a morphing face
  inside fog, depth of field across a camera cut, the denoiser on volumes. This task makes a short
  film and turns it into the regression suite that protects everything built.
- **Phases:**
  1. *Analysis:* storyboard three shots (about 240 frames at 24 fps) that use a character with
     facial animation, a moving camera with depth of field, a cut, volumetric light, layered
     materials, an Alembic cloth cache and many lights.
  2. *Decision:* the quality settings (spp, denoising, resolution) for an agreed time budget per
     frame, and which frames become golden references.
  3. *Build:* the assets, the sequence file, and the CTest that renders the golden frames.
  4. *Diagnose:* at least one interaction bug will show up (e.g. depth of field on denoised
     volumes, motion vectors across the cut). Trace it to the owning task and fix it there.
  5. *Verify:* see the pass check.
- **Decision:** what counts as a regression (Task 01 limits per golden frame); how long the CI
  version may take (lower resolution, fewer spp, same frames).
- **CLion:** compound run configurations (render, then encode); CTest labels to run the cinematic
  suite apart from the fast tests; *Local History* if an asset change breaks a golden frame.
- **Other tools:** Blender for assets; FFmpeg; FLIP; tev.
- **Pass check:** the full film renders unattended with `dream render`, encodes to a correctly
  tagged video, and its golden frames pass; render time per frame is reported; a written review
  lists what still looks below film quality, which becomes the next plan.
- **Output:** a short film, and a regression suite for the cinematic renderer.

---

## The chain at a glance

| # | Task | Takes from | Gives to |
|---|---|---|---|
| 01 | Perceptual image judge | screenshot checks | every check |
| 02 | Deterministic offline time | 01 | 03, 12, 13, 16, 32, 34 |
| 03 | HDR read-back and EXR | 02 | 04, 09, 33, 35 |
| 04 | Compute: design and Vulkan | 03 | 05 |
| 05 | Compute on the other backends | 04 | 06, 18, 30 |
| 06 | Render graph core | 05 | 07 |
| 07 | Forward renderer on the graph | 06 | 08 |
| 08 | Transient memory aliasing | 07 | 18 |
| 09 | Working colour space | 03 | 10 |
| 10 | Colour management module | 09 | 11, 25, 35 |
| 11 | Physical camera and light units | 10 | 12, 16, 17 |
| 12 | Animation curves | 11, 02 | 13 |
| 13 | Sequencer: shots and cuts | 12 | 14 |
| 14 | Motion vectors | 13 | 15, 18, 29, 30 |
| 15 | TAA for the preview | 14 | 16 |
| 16 | Offline accumulation and motion blur | 15, 11, 02 | 17, 22, 24, 25, 32 |
| 17 | Depth of field | 16 | 18 |
| 18 | `cinematic` renderer, visibility buffer | 08, 14, 17 | 19, 21, 23, 29 |
| 19 | Energy-conserving GGX | 18 | 20 |
| 20 | Layered materials | 19 | 21, 22 |
| 21 | Subsurface scattering | 20 | 25 |
| 22 | Hair shading | 20, 16 | 25 |
| 23 | Ray tracing in the RHI | 18 | 24 |
| 24 | Acceleration structures for animation | 23, 16 | 25, 30, 32 |
| 25 | Path tracer core | 24, 19–22, 10 | 26 |
| 26 | Many lights | 25 | 27 |
| 27 | Robustness: acne, fireflies, NaNs | 26 | 28 |
| 28 | Volumes | 27 | 29 |
| 29 | Denoiser module | 28, 18, 14 | 33 |
| 30 | Morph targets | 14, 24 | 31 |
| 31 | Dual-quaternion skinning | 30 | 32 |
| 32 | Alembic caches | 31, 16, 24 | 37 |
| 33 | AOVs, multi-layer EXR, Cryptomatte | 29, 03 | 34 |
| 34 | Batch renderer `dream render` | 33 | 35 |
| 35 | Video encoding | 34, 10 | 36 |
| 36 | Path tracer performance | 35 | 37 |
| 37 | Capstone cinematic | all | — |

Tasks 30–32 (character animation) depend only on 14, 16 and 24, so they can be done in parallel
with Milestone F if two people work on the plan.

## New modules this plan adds

| Interface | Backends | Task |
|---|---|---|
| `image_writer` | `exr`, `png`, `null` | 03 |
| `color_management` | `ocio`, `builtin`, `null` | 10 |
| `sequencer` | `curves`, `null` | 13 |
| `renderer` (existing) | new module `cinematic` | 18 |
| `denoiser` | `oidn`, `null` | 29 |
| `geometry_cache` | `alembic`, `null` | 32 |
| `video_encoder` | `ffmpeg_process`, `null` | 35 |

RHI additions (compute, storage resources, ray tracing) are capabilities of the existing `rhi`
interface, reported by `supports_*()` so that every module can fall back.
