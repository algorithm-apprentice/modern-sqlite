# ADR-0009: Build and Toolchain Baseline

- Status: Accepted
- Date: 2026-10-03

## Context

The project needs a portable C++23 build, a fast local TDD loop, deterministic
test selection, sanitizer configurations, and machine-readable presets that
work consistently for people and AI agents.

The build must not hide missing tests, silently skip unavailable tools, or
require an IDE-specific project format as the source of truth.

## Decision

- CMake is the canonical build-system generator.
- Ninja is the default local and CI build tool; other CMake generators remain
  supported when they preserve behavior.
- CMake presets define supported configure, build, test, sanitizer, and
  benchmark workflows.
- CTest is the canonical test entry point.
- GoogleTest is the initial C++ unit-test framework and is pinned to an exact
  reviewed version.
- Dependency acquisition is explicit and reproducible. Ordinary builds must
  never float to a newer dependency revision.
- Clang and GCC are supported on Linux; Apple Clang is supported on macOS.
- `clang-format` defines source formatting.
- `clang-tidy`, compiler warnings, ASan, UBSan, and TSan are separate explicit
  validation configurations rather than implicit best-effort checks.
- Selecting zero tests is an error in repository automation.
- Benchmark binaries and instrumentation are separate from the ordinary
  production library.

Exact minimum tool versions and pinned dependency revisions will be recorded by
the `bootstrap-build` node after testing the supported environments.

## Consequences

- IDE integrations consume the canonical CMake project rather than defining
  independent build logic.
- Build and test commands can be referenced by agents and CI without
  reconstructing environment-specific flags.
- New dependencies require an ADR or an amendment to an accepted dependency
  decision.
- Missing optional validation tools fail the explicit validation command
  clearly but do not masquerade as a successful check.
