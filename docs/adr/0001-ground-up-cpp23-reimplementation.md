# ADR-0001: Ground-Up C++23 Reimplementation

- Status: Proposed
- Date: 2026-10-03

## Context

SQLite is proven and highly optimized, but its C implementation, master
internal header, mutable cross-layer structures, generated amalgamation, and
compatibility history are not the target architecture for this project.

The new repository has no deployed users, production databases, or binary
compatibility commitments.

## Decision

Modern SQLite will be implemented from an empty source tree using C++23.

The pinned SQLite source is:

- A behavioral specification.
- A persistent-format reference.
- A source of test scenarios and golden vectors.
- A differential testing oracle.
- A performance baseline.

The implementation will not be a line-by-line translation and will not retain
SQLite source boundaries when they conflict with the target dependency graph.

## Consequences

- `std::expected`, RAII, strong types, spans, variants, and explicit move-only
  handles can define the core contracts.
- Production readiness requires extensive interoperability and crash evidence.
- Behavioral or format differences must be intentional and documented.
- The project may remain pre-alpha for a long time while durability and SQL
  semantics are validated.
