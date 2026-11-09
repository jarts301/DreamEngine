# Architecture decision records

Each file records one decision that shapes the engine: what was decided, why, and what it costs.
Changing a decision means adding a new record that supersedes the old one, not rewriting history.

| ADR | Decision |
|---|---|
| [0001](0001-layered-architecture.md) | Layers, dependency direction and module rules |
| [0002](0002-cpp-subset.md) | The C++20 subset the engine uses |
| [0003](0003-naming-and-layout.md) | Naming and repository layout |
| [0004](0004-initial-targets.md) | First targets: 3D, SDL3 backend, Android API 29 |
| [0005](0005-core-design-choices.md) | Math conventions, configuration format, job system |
| [0006](0006-module-system.md) | Module selection, registration and the layer check |

## Template

```markdown
# NNNN. Title

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context
## Decision
## Consequences
```
