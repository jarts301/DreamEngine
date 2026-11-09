# 0003. Naming and repository layout

- Status: Accepted
- Date: 2026-11-09

## Decision

### Code

| Element | Style | Example |
|---|---|---|
| Namespaces | `dream::lowercase` | `dream::core` |
| Types and concepts | `PascalCase` | `JobSystem` |
| Functions and variables | `snake_case` | `engine_version` |
| Private data members | `m_` prefix | `m_capacity` |
| Constants | `k_` prefix | `k_max_threads` |
| Macros | `DREAM_` prefix, upper case | `DREAM_ASSERT` |
| Files | `snake_case` | `job_system.cpp` |
| Headers | `#pragma once` | |

Avoid the identifiers `major` and `minor`: Android's system headers can define them as macros.

Formatting is defined by `.clang-format`.

### Repository

| Path | Contents |
|---|---|
| `core/` | Core library |
| `interfaces/<name>/` | One interface per subsystem |
| `modules/<interface>/<backend>/` | One backend per directory, e.g. `modules/platform/sdl3/` |
| `runtime/` | Engine runtime |
| `samples/` | Sample games |
| `tools/` | Offline tools |
| `cmake/` | Shared build scripts |
| `docs/adr/` | Decision records |

Inside a library, public headers go in `include/dream/<library>/`, sources in `src/`, and tests in
`tests/`.

### Build targets

Targets are named `dream_<name>` with an alias `dream::<name>`. Other targets link to the alias.

## Consequences

A backend's location states which interface it implements, so listing `modules/` shows every
available option.
