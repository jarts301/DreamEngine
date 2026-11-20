# Modules

Backends that implement the contracts in `interfaces/`, stored as `modules/<interface>/<backend>/`.
A module depends only on interfaces and Core. See
[ADR 0001](../docs/adr/0001-layered-architecture.md) and
[ADR 0006](../docs/adr/0006-module-system.md).

## Adding a backend

`modules/greeter/english/` is the smallest working example. A backend needs two things:

1. A `CMakeLists.txt` that calls `dream_add_module`, which is the only build setup required:

   ```cmake
   dream_add_module(physics2d box2d SOURCES src/box2d_physics.cpp LIBRARIES box2d::box2d)
   ```

2. A source file that defines the registration function, named after the interface and backend:

   ```cpp
   namespace dream::modules {

   core::Result<void> register_physics2d_box2d(core::ModuleRegistry& registry) {
       return registry.add(core::ModuleDescriptor<physics2d::PhysicsWorld>{
           .name = "box2d",
           .create = &create,
           .is_supported = &is_supported, // Optional.
       });
   }

   } // namespace dream::modules
   ```

Then list it in a project's TOML file. Nothing else needs to change.

On this branch (ADR 0039) there are no shaders: the `software` rhi module draws nothing itself,
and the `software` renderer draws on the CPU through `Device::software_access()`. A third-party
library a backend needs goes, as source, into `third_party/`, with its license, so the build never
downloads anything.

Third-party code must be under a permissive license: MIT, Apache 2.0, BSD, zlib or public domain
([ADR 0022](../docs/adr/0022-licenses-and-frozen-libraries.md)). A backend can also build on one
of the engine's own [libraries](../libraries/README.md), linked as `dream::library_<name>`, and
stay a thin adapter; those are frozen, so new backends build on third-party code.

Interfaces with more than one backend can share tests. Every physics backend passes the tests in
`physics3d/tests` or `physics2d/tests`, and every profiler backend those in `profiler/tests`, which
its own test program includes.
