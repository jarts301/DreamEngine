# 0013. DirectX backend and per-backend shader formats

- Status: Accepted
- Date: 2026-11-10

## Context

ADR 0001 requires a graphics API to be replaceable by changing the project configuration alone.
ADR 0008 built the graphics interface with a Vulkan backend, compiled every shader to SPIR-V, and
left room for "a future backend to ship its own format". Adding a DirectX backend for PC is the
first real test of both: it needs another compiled shader format, and it has no push constants or
vertex attribute locations, which the shaders and the interface took for granted.

## Decision

### The backend

`modules/rhi/directx` implements the interface on Direct3D 11, feature level 11.0, with a DXGI
flip-model swapchain, so it runs on any Windows 10 PC. Direct3D 11 was chosen over 12 because the
interface is small and 11 already provides what the Vulkan backend had to build itself: memory
management, synchronization, resource renaming for buffers rewritten between draws, and mipmap
generation. The result is a backend a fraction of the size of the Vulkan one, and a second real
implementation of the interface (ADR 0001, rule 3). A Direct3D 12 backend can arrive as another
module when a feature needs it.

The backend builds only on Windows and is selected like any other backend:

```toml
[modules.windows]
rhi = ["vulkan", "directx", "null"]
```

Settings under `[rhi.directx]`: `debug` turns the debug layer on (the default in debug builds, and
its messages go to the engine log), and `software` uses WARP, Windows' software rasterizer.

Direct3D's clip space already matches ADR 0005, so unlike Vulkan nothing is flipped.

### One shader format per backend

A graphics backend declares the compiled shader format it loads when it registers with the build:

```cmake
dream_add_module(rhi directx ... SHADER_SUFFIX .dxbc SHADER_TARGET dxbc SHADER_PROFILE sm_5_0)
```

`dream_link_modules` records which backends a game selected, and `dream_add_game` compiles the
game's shaders once for every format those backends load, each under its own suffix. Listing
`directx` therefore makes `.dxbc` files appear next to the `.spv` files without touching any build
script, and a game that lists only `null` still gets SPIR-V, which the null device loads.

DirectX shaders are DXBC for shader model 5.0, produced by slangc through fxc, which every Windows
10 machine has. No new dependency is installed.

### Shader bindings on both APIs

The interface's single layout (ADR 0010) maps to fixed DirectX registers, and every shader states
both sides:

| Resource | Vulkan | DirectX | Slang declaration |
|---|---|---|---|
| Uniform block (`set_uniforms`) | binding 0, set 0 | `b0` | `[[vk::binding(0, 0)]] ConstantBuffer<T> x : register(b0);` |
| Texture (`bind_texture`) | binding 1, set 0 | `t0` and `s0` | `[[vk::binding(1, 0)]] Sampler2D x;` |
| Push constants | push constant block | `b1` | `[[vk::push_constant]] ConstantBuffer<T> x : register(b1);` |

Slang applies `[[vk::...]]` only to SPIR-V and `register(...)` only to DirectX. The texture needs
no register, because a shader has one and Slang gives it `t0`/`s0`. Shaders that declare a
constant buffer anywhere else are refused by the DirectX backend with a message naming the rule.

DirectX matches vertex attributes to shader inputs by semantic name rather than by location. The
interface keeps locations, and the backend reads the vertex shader's input signature to translate:
a shader declares its vertex inputs in location order, and the n-th input is location n. Every
shader in the repository already did this, and the backend reports a mismatch at pipeline creation.

## Consequences

- A project on Windows can switch graphics APIs, or fall back from one to the other, by editing its
  module list. The samples list `directx` after `vulkan`, and each one that has a Vulkan check run
  has a DirectX one, built only on Windows.
- Shaders carry one extra `register(...)` per constant buffer. Slang's SPIR-V path reads that
  register as an implied Vulkan binding, as DXC does, and warns that the push constants overlap the
  texture even though a push constant has no binding. `dream_add_game` disables that one warning,
  which also hides a real overlap of explicit bindings; Vulkan's validation layers still catch
  those.
- A game's shaders are compiled once per selected format, so a Windows build that lists both
  backends compiles each shader twice.
- Resources are destroyed without waiting for the GPU, because Direct3D 11 keeps them alive while
  commands still use them. The interface's "may wait" wording still holds for Vulkan.
- Windows build servers need no GPU for this backend: `dream_rhi_directx_tests` runs it on WARP,
  Windows' software rasterizer, drawing to a hidden window, and compiles its own HLSL. What it
  can't judge, the picture, is checked by running the `*_directx_check` programs on a real PC.
