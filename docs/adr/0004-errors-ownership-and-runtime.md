# ADR-0004: Errors, Ownership, and Runtime Boundaries

- Status: Accepted
- Date: 2026-10-03

## Context

Database failures such as I/O errors, corruption, busy locks, constraint
violations, and cancellation are expected outcomes. Exceptions, ambiguous
nulls, shared ownership by default, and broad recovery catches would obscure
durability and lifetime behavior.

## Decision

- Expected failures return `std::expected<T, Error>` or an equivalent project
  alias.
- Error categories preserve SQLite-compatible primary and extended codes where
  compatibility requires them.
- Exceptions are not used for normal storage-engine control flow.
- `std::bad_alloc` and foreign callback exceptions are contained only at
  explicit API boundaries and converted to typed errors.
- Resources use RAII and move-only ownership.
- Pins and borrowed views are represented by types with documented lifetimes.
- Owning raw pointers are forbidden.
- External bytes are validated once at trust boundaries.
- Established internal invariants use assertions instead of repeated
  recoverable validation in hot paths.
- No catch-all handler may convert an unknown failure into success.

## Consequences

- Failure paths remain visible in signatures and tests.
- Hot paths can avoid repeated defensive work only after an invariant is
  documented and tested.
- Public and callback boundaries must define exception and allocation behavior.
- The error model can later map to SQLite C result codes without leaking C API
  ownership into lower layers.
