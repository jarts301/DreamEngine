# 0002. C++20 subset

- Status: Accepted
- Date: 2026-11-09

## Context

The engine and games are C++20 on every platform. Compilers differ in what they actually support:
MSVC's standard library and the Android NDK's libc++ are not at the same level, and consoles often
forbid exceptions and RTTI.

## Decision

Standard: C++20, compiler extensions off, warnings treated as errors in every preset.

| Allowed | Notes |
|---|---|
| Concepts, `<=>`, `consteval`, designated initializers | Language features |
| `<span>`, `<bit>`, `<concepts>`, `<compare>`, `<source_location>` | Library features |
| `<format>`, `<ranges>` | Keep out of hot paths |

`core/tests/cpp20_baseline_test.cpp` exercises this list, so CI fails on any toolchain that cannot
compile it. Add to the test when adding to the list.

| Not allowed | Reason |
|---|---|
| Named modules (`import`, `export module`) | Toolchain and build-system support is uneven, especially on Android |
| Exceptions | Disabled by compiler flag; return `Result<T>` from fallible functions |
| RTTI (`dynamic_cast`, `typeid`) | Disabled by compiler flag |
| `std::jthread`, `std::stop_token` | Not dependable in the NDK's libc++; Core will provide threading |
| Parallel algorithms, `<chrono>` calendars and time zones | Uneven support; use the job system and Core's clock |

Tests compile with the same flags as the engine, so they exercise the configuration that ships.

## Consequences

- Third-party libraries that require exceptions must be contained inside a module or avoided.
- `Result<T>` must exist before the first fallible API. Core provides it in its first iteration.
