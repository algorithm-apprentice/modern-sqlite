# ADR-0007: Rollback Journal Before WAL

- Status: Proposed
- Date: 2026-10-03

## Context

SQLite supports rollback journals, WAL, multiple locking modes, shared memory,
checkpoints, and concurrent readers. Implementing these together would combine
file format, locking, recovery, and concurrency risks before basic B-tree and
SQL behavior are proven.

Rollback mode exposes the fundamental pager invariants and supports a useful
single-connection database without requiring WAL-index shared memory.

## Decision

Implementation order is:

1. Read-only pager and B-tree interoperability.
2. Read-only SQL execution.
3. Journal backend contracts.
4. Rollback-journal DELETE mode.
5. Writable pager and B-tree.
6. Single-database, single-connection writable MVP.
7. Multi-connection locking.
8. WAL frames, WAL-index, snapshots, and checkpointing.

The pager depends on a journal contract, not on WAL-specific concrete types.
WAL is a later backend and integration node.

## Consequences

- Atomic commit and crash recovery are validated before WAL complexity.
- The first writable MVP remains intentionally smaller than SQLite.
- Pager interfaces must anticipate backend substitution without introducing a
  general plugin framework.
- WAL performance and concurrency claims are deferred until the locking model
  exists.
