# Interfaces

Abstract contracts for swappable subsystems, one directory each. No implementation code and no
backend types. See [ADR 0001](../docs/adr/0001-layered-architecture.md).

## Adding an interface

`greeter/` is the smallest working example. An interface needs a directory added to
`CMakeLists.txt` here, a one-line `CMakeLists.txt` calling `dream_add_interface(<name>)`, and an
abstract class in `include/dream/<name>/` with a virtual destructor and a
`static constexpr std::string_view k_module_interface` holding its configuration name.
