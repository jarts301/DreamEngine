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

Interfaces with more than one backend can share tests. Every physics backend passes the tests in
`physics3d/tests` or `physics2d/tests`, which its own test program includes.
