# Modules

Backends that implement the contracts in `interfaces/`, stored as `modules/<interface>/<backend>/`.
A module depends only on interfaces and Core. See
[ADR 0001](../docs/adr/0001-layered-architecture.md) and
[ADR 0006](../docs/adr/0006-module-system.md).

## Adding a backend

`modules/greeter/english/` is the smallest working example. A backend needs two things:

1. A `CMakeLists.txt` that calls `dream_add_module`, which is the only build setup required:

   ```cmake
   dream_add_module(physics3d jolt SOURCES src/jolt_physics.cpp LIBRARIES Jolt::Jolt)
   ```

2. A source file that defines the registration function, named after the interface and backend:

   ```cpp
   namespace dream::modules {

   core::Result<void> register_physics3d_jolt(core::ModuleRegistry& registry) {
       return registry.add(core::ModuleDescriptor<physics3d::PhysicsWorld>{
           .name = "jolt",
           .create = &create,
           .is_supported = &is_supported, // Optional.
       });
   }

   } // namespace dream::modules
   ```

Then list it in a project's TOML file. Nothing else needs to change.

A program usually creates one backend per interface, the first in its list that starts. A few
interfaces are used several at once, such as the editor's panels, `editor_panel`: there the program
calls `App::create_modules`, which creates every backend listed, in order
([ADR 0046](../docs/adr/0046-editor-v1.md)).

A graphics backend also declares the compiled shader format it loads, and `dream_add_game` compiles
each game's shaders for every format its selected backends need:

```cmake
dream_add_module(rhi directx SOURCES src/directx_device.cpp LIBRARIES d3d11 d3dcompiler dxguid
    SHADER_SUFFIX .dxbc SHADER_TARGET dxbc SHADER_PROFILE sm_5_0)
```

Backends that load the same format, such as `directx` and `directx12`, declare it alike, and the
shaders are compiled once. The targets `glsl` and `essl`, with the GLSL version as the profile, go
through SPIR-V and SPIRV-Cross instead of straight from Slang
([ADR 0026](../docs/adr/0026-opengl.md)). See [ADR 0013](../docs/adr/0013-directx-and-shader-formats.md) for the
shader bindings every backend shares.

A module that draws brings its own Slang shaders with `SHADERS`. Every game that links the module
gets them compiled with its own, for each of its graphics backends:

```cmake
dream_add_module(renderer forward SOURCES src/forward_renderer.cpp
    SHADERS shaders/forward.slang shaders/shadow.slang shaders/tonemap.slang)
```

The `renderer` interface works this way ([ADR 0030](../docs/adr/0030-renderer-module.md)).

Third-party code must be under a permissive license: MIT, Apache 2.0, BSD, zlib or public domain
([ADR 0022](../docs/adr/0022-licenses-and-frozen-libraries.md)). A backend can also build on one
of the engine's own [libraries](../libraries/README.md), linked as `dream::library_<name>`, and
stay a thin adapter; those are frozen, so new backends build on third-party code.

Interfaces with more than one backend can share tests. Every physics backend passes the tests in
`physics3d/tests` or `physics2d/tests`, and every profiler backend those in `profiler/tests`, which
its own test program includes.
