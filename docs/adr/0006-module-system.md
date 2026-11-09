# 0006. Module system

- Status: Accepted
- Date: 2026-11-09

## Context

[ADR 0001](0001-layered-architecture.md) requires backends to be chosen from one project
configuration file, at build time and at run time. It also requires the layer rules to be checked
automatically.

## Decision

### Selection

A project's TOML file has one table per platform. Each list is a priority order:

```toml
[modules.windows]
physics = ["jolt", "bullet"]
```

| When | What happens |
|---|---|
| Configure | `dream_link_modules` adds only the listed backends to the build and generates their registration code. |
| Startup | `ModuleProject::create<Interface>()` tries the list in order and uses the first backend that is linked, supported and starts successfully. |

Rejected backends are logged, and if none is usable the error gives the reason for each one.

### One file, read twice

CMake has no TOML parser. `DreamModuleConfig.cmake` reads a strict one-line form of the
`[modules.<platform>]` tables and stops with an error on anything else, including forms that are
valid TOML, such as multi-line arrays. Refusing them means CMake can never quietly read a list
differently from the real parser.

The file's text is embedded in the generated source and parsed by `Config` at startup. This needs no
file access, so it works the same on Android before the platform layer exists.

### Registration

Each backend defines `dream::modules::register_<interface>_<backend>`. The generated source calls
these directly, so linkers can't strip them from static libraries. No global constructors are
involved, and a missing function is a link error.

The registry lives in Core because modules may depend only on Interfaces and Core. It knows nothing
about specific interfaces. Factories are stored type-erased and checked against a per-interface tag,
because RTTI is disabled.

There is no interface version number. Everything is compiled from source together, so the compiler
catches a mismatch. Dynamic loading would need one if it is ever added.

### Layer check

`tools/check_layers.py` runs in CI and checks `#include` lines and `dream::` CMake targets against
the rules in ADR 0001. It needs only Python's standard library, and the engine build does not depend
on it.

## Consequences

- Changing a list requires a CMake re-run, which the build triggers automatically, and a rebuild.
  Reordering a list can't yet be done on an installed game, because the file is embedded.
- Project configuration files are limited to 16000 bytes, the largest string literal that MSVC
  reliably accepts.
- The `greeter` interface and its two backends exist only to prove and test this path. They are
  the template for real interfaces, and can be removed once real interfaces cover the same ground.
- A backend's third-party libraries are always installed by vcpkg even when it isn't selected, until
  backends with such dependencies arrive and need vcpkg features.
