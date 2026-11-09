# 0001. Layered architecture and module rules

- Status: Accepted
- Date: 2026-11-09

## Context

Any subsystem (graphics API, physics, UI, platform layer) must be replaceable by changing a
configuration, and new platforms must be cheap to add. That only holds if dependencies are
controlled from the first commit.

## Decision

The engine has five layers. Dependencies point downward only.

| Layer | Directory | Contents | Swappable |
|---|---|---|---|
| Game | `samples/`, external projects | Game code, built as a library | n/a |
| Runtime | `runtime/` | App loop, scene, ECS, asset manager | No |
| Modules | `modules/` | Backends such as Vulkan, Jolt, SDL3 | Yes |
| Interfaces | `interfaces/` | Abstract contracts, no implementation | Stable contracts |
| Core | `core/` | Memory, logging, math, jobs, config | No |

Rules:

1. Backend types never appear in an interface header.
2. A module depends only on interfaces and Core, never on another backend.
3. An interface is not finished until it has two real implementations.
4. Games never include platform or backend headers.
5. Backend-specific features go through an optional capability query, not the main interface.

Selection works at two points, driven by one project configuration file:

- **Build time:** only the listed modules are compiled, and a registration source file is
  generated. Registration is generated because linkers can strip self-registering code from static
  libraries.
- **Run time:** a priority list decides which compiled-in backend starts, with fallback.

Modules link statically by default, because iOS and consoles restrict dynamic loading. Dynamic
loading may be added later as a PC-only development feature; it is not a stable plugin ABI.

Core and the ECS are deliberately not swappable. They are the vocabulary every module shares.

## Consequences

- Each new subsystem costs an interface plus a backend, even when only one backend exists at first.
- Code under NDA, such as console backends, can live in private out-of-tree modules.
- These rules are conventions until the module system adds an automated dependency check in CI.
