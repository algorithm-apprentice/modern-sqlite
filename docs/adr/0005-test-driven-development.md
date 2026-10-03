# ADR-0005: Test-Driven Development

- Status: Accepted
- Date: 2026-10-03

## Context

A database may appear correct while containing defects in malformed-input
handling, state transitions, transaction ordering, ownership, recovery, or SQL
edge semantics. Tests added after implementation cannot reliably distinguish
specified behavior from accidental behavior.

The inner loop must remain fast; crash campaigns, fuzzing, sanitizers, and
benchmarks are essential but are not substitutes for focused unit tests.

## Decision

New or changed executable behavior follows red-green-refactor:

1. Write one focused test for observable behavior.
2. Run it and observe the expected failure.
3. Implement the smallest correct behavior.
4. Run the focused test and all fast unit tests.
5. Refactor only while green.

Tests are part of each production DAG node. Applicable suites include:

- `unit`: deterministic contracts and state transitions.
- `format`: independent SQLite golden vectors.
- `parser`: syntax, diagnostics, and malformed SQL.
- `differential`: rows, types, errors, and effects against pinned SQLite.
- `crash`: deterministic VFS failures and power-loss states.
- `model`: seeded state-machine comparisons.
- `fuzz`: decoders, parser, planner, VM, and stateful database operations.
- `performance`: throughput, CPU, allocation, I/O, and work-count baselines.

Behavior-preserving refactors start and remain green. Build-system changes use
configure/build/test integration checks. Documentation-only changes use
document review rather than artificial unit tests.

## Consequences

- Interfaces must support dependency injection and deterministic failure.
- Persistent formats begin with golden vectors, not self-generated round trips.
- Fast unit tests avoid sleeps, large databases, and unnecessary process setup.
- A DAG node cannot be complete based only on compilation.
