# ADR-0002: SQLite Compatibility Contract

- Status: Accepted
- Date: 2026-10-03

## Context

A new file format or unrelated SQL dialect would remove the strongest
available oracles for storage, recovery, semantics, and performance. Full
SQLite API and feature compatibility, however, is too large for the initial
milestones.

SQLite documents the database file format as stable. SQLite also documents
VDBE bytecode as an internal implementation detail that changes between
releases.

## Decision

Compatibility priority is:

1. SQLite 3 database-file and record formats.
2. Transaction atomicity, recovery, locking, and corruption behavior.
3. Observable SQL rows, value types, errors, and side effects for implemented
   features.
4. Bidirectional interoperability with the pinned SQLite reference.
5. C API compatibility only after the native C++ API and engine are stable.

SQLite VDBE opcode compatibility, source compatibility, and ABI compatibility
are not initial requirements.

Compatibility is incremental. Unsupported SQL must fail explicitly rather than
silently changing semantics.

No generic migration framework will be built before an accepted feature
requires an incompatible persistent representation.

## Consequences

- SQLite can generate golden databases, records, and expected results.
- Modern SQLite output can be checked with upstream queries and
  `PRAGMA integrity_check`.
- Internal bytecode and planner design remain free to use typed C++ contracts.
- Every supported compatibility surface requires an explicit matrix.
