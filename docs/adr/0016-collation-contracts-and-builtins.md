# ADR-0016: Collation Contracts and Built-ins

- Status: Accepted
- Date: 2026-10-03

## Context

Expression comparison, index keys, grouping, uniqueness, and ordering need one
shared text-ordering contract. SQLite 3.54.0 provides the built-in `BINARY`,
`NOCASE`, and `RTRIM` collations and permits connection-local custom
collations. Its comparison callback receives explicit byte lengths, cannot
report an error, and must define a deterministic total ordering.

Modern SQLite already preserves arbitrary text bytes, including malformed
UTF-8 and embedded NUL. Collation therefore cannot assume validated Unicode or
NUL-terminated input. The runtime path is performance-sensitive and must not
allocate, normalize, or decode text merely to compare it.

## Decision

- `Collation` is an immutable, non-copyable polymorphic contract with a stable
  name and a `noexcept` weak ordering over two borrowed `Utf8View` values.
- Custom implementations own any required state and must remain alive for
  every borrowed use. They must return a deterministic weak ordering and obey
  symmetry and transitivity.
- Built-in collations are process-wide immutable singletons:
  - `BINARY` compares unsigned bytes with `memcmp()` semantics and orders a
    shorter equal prefix first.
  - `NOCASE` folds only ASCII `A` through `Z`, leaves every other byte
    unchanged, and preserves SQLite's embedded-NUL behavior.
  - `RTRIM` removes only trailing ASCII space bytes (`0x20`) before applying
    `BINARY`; tabs, other whitespace, and non-ASCII spaces are significant.
- Collation-aware SQL-value comparison applies the selected collation only
  when both operands are TEXT. NULL, numeric, TEXT-versus-BLOB, and BLOB
  ordering continue to use the SQL-value contract from ADR-0015.
- SQL comparison overloads preserve ordinary NULL propagation and `IS` /
  `IS NOT` semantics while using the selected collation for TEXT equality and
  ordering.
- Comparison performs no allocation, UTF-8 validation, Unicode
  normalization, locale lookup, or text transcoding.
- This node defines UTF-8 comparison contracts only. Connection-local
  registration, case-insensitive name lookup, replacement invalidation,
  collation-needed callbacks, UTF-16 adapters, and SQL collation-selection
  rules belong to later session, binder, and C API nodes.

## Consequences

- Record, B-tree, function, binder, and VM modules can share stable collation
  identities without depending on parser or connection internals.
- Built-in comparisons match SQLite byte-for-byte, including malformed UTF-8
  and the unusual `NOCASE` treatment of embedded NUL.
- The hot path requires one virtual dispatch only for TEXT-to-TEXT comparison;
  all other storage-class comparisons bypass collation dispatch.
- Custom collations cannot surface runtime failures through comparison.
  Invalid ordering implementations violate the contract, as they do in
  SQLite.
