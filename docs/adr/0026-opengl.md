# 0026. OpenGL and OpenGL ES backends

- Status: Accepted
- Date: 2026-11-18

## Context

The engine drew with Vulkan everywhere and with Direct3D 11 and 12 on Windows. Phones without a
working Vulkan driver, and PCs with old or broken drivers, had nothing but the null backend. OpenGL
ES runs on every Android phone, and OpenGL on every Windows PC with a GPU driver, so they make the
widest fallback.

## Decision

Two modules, built from one source file, `modules/rhi/opengl/src/gl_device.cpp`:

| Module | API | Platforms | Shaders |
|---|---|---|---|
| `opengl` | OpenGL 4.3 core | Windows | `.glsl`, `#version 430` |
| `opengles` | OpenGL ES 3.1 | Android, and Windows where the driver makes ES contexts | `.essl`, `#version 310 es` |

They are listed like any backend, usually after the others:

```toml
[modules.windows]
rhi = ["vulkan", "directx12", "directx", "opengl", "opengles", "null"]

[modules.android]
rhi = ["vulkan", "opengles", "null"]
```

`[rhi.opengl] debug` and `[rhi.opengles] debug` turn on a debug context and log the driver's errors
and warnings; it is the default in debug builds.

### Why these versions

OpenGL 4.3 and OpenGL ES 3.1 are the first versions that share what the device needs: explicit
bindings for uniform blocks and textures, and locations for every shader input and output, so a
shader's layout is fixed when it is compiled, as on the other APIs; vertex formats separate from
buffers; immutable texture storage; and samplers. ES 3.1 reaches nearly every Android phone made
since 2015, and 4.3 every Windows GPU driver of the last decade.

### The context belongs to the device

The device makes its own context from the window handle it is given, like a Vulkan surface: through
WGL on Windows and EGL on Android. The platform layer has nothing to add, and a game that lists both
OpenGL and another backend can fall back from one to the other. The context exists before any window
and outlives it, current on a hidden window (WGL) or a 1x1 pbuffer (EGL), because the interface
creates resources before attaching a surface and keeps them when Android takes the surface away.

### Shaders

Slang can't write GLSL ES, so both kinds are compiled through SPIR-V: Slang writes SPIR-V, and
SPIRV-Cross (Apache 2.0, a vcpkg host tool) turns it into GLSL, in
`cmake/DreamCrossCompileShader.cmake`. The shader sources are unchanged, and the bindings of
ADR 0013 carry over:

| | Vulkan | DirectX | OpenGL |
|---|---|---|---|
| Uniform block | set 0, binding 0 | `b0` | block binding 0 |
| Texture | set 0, binding 1 | `t0`, `s0` | texture unit 1 |
| Push constants | push constant block | `b1` | block binding 2 |

OpenGL has no push constants, so they become a uniform block. SPIR-V gives that block no binding,
and the build step gives it binding 2. Two other conversions happen there:

- The engine's clip space has depth from 0 to 1, as Vulkan's and Direct3D's do, and OpenGL's goes
  from -1. SPIRV-Cross's `--fixup-clipspace` converts it in the vertex shader. Its +Y is up, as
  OpenGL's is, so nothing flips.
- SPIRV-Cross writes the math for push constants as for a plain column-major uniform, reversing the
  multiplication of a row-major matrix, but still marks the matrix `row_major` in the block, which
  would transpose it twice. The build step removes the mark from that block.

Push constants are declared std430 and read as std140. The two differ only for arrays and nested
structs, which the engine's push constants don't have; Direct3D's constant buffer packing already
rules them out too.

The device checks a linked program's bindings and inputs, as the DirectX device checks a shader's
registers, so a shader that reads the wrong binding is an error rather than garbage.

### sRGB windows

The window must store sRGB-encoded colors, as the other backends' windows do. Desktop OpenGL gets
an sRGB-capable pixel format and `GL_FRAMEBUFFER_SRGB`, and EGL an sRGB window surface through
`EGL_KHR_gl_colorspace`. Drivers don't all honor these, and answer the query for it unreliably, so
the device tries: it clears one pixel to a known linear gray and reads it back. If the window
didn't encode it, frames are drawn to an offscreen sRGB texture, and a last pass copies it to the
window, encoding in the shader. That costs one full-screen pass per frame where it is needed.

### Memory and timing

Constants are written into a 1 MB ring buffer renewed each frame, one slot per call, so each draw
keeps the values it was given. Per-frame vertices use a 2 MB buffer renewed the same way. GPU time
comes from time-elapsed queries, which ES needs `EXT_disjoint_timer_query` for. Compressed
textures follow the driver: BC7 is core in OpenGL 4.2, and ASTC in OpenGL ES 3.2.

## Consequences

- Every sample's checks pass on both backends on an RTX 5080: the screenshot checks of the cube,
  render demo, arcade and Toppler match the same reference images as Vulkan and Direct3D, the
  physics demo's script passes, and Toppler clears its ten levels. NVIDIA's ES contexts don't
  encode sRGB, so `opengles` on that PC uses the offscreen pass.
- On a Galaxy A57, `opengles` draws the render demo as Vulkan does, with ASTC textures. Samsung's
  OpenGL ES there is ANGLE running on Vulkan. Toppler keeps drawing after leaving and returning to
  the app.
- The device tests, `dream_rhi_opengl_tests` and `dream_rhi_opengles_tests`, need a GPU driver.
  Build servers have none, so there they report that the device can't start and pass.
- When Android takes a window away, OpenGL ES may log errors for the frames drawn before the game
  hears of it, which Vulkan reports quietly as an out-of-date swapchain. They stop with the pause.
- Linux has no OpenGL backend yet; it would need GLX or EGL on X11 and Wayland in the context class.
- OpenGL ES 3.0 phones, without 3.1, are left out. Supporting them would mean binding blocks and
  textures by name after linking, since 3.0's shaders can't declare bindings.
