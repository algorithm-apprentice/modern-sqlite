# Implementation Dependency DAG

## Purpose

Modern SQLite is implemented as a directed acyclic graph of independently
verifiable nodes. Dependency edges are technical prerequisites, not permission
to develop multiple nodes in parallel.

Only one node is developed at a time. Independent ready nodes remain pending
until the current node is complete and reviewed.

The machine-readable source is `project/module-graph.json`. This document
explains the canonical linearization and completion gates.

## Canonical topological order

1. `document-architecture`
2. `bootstrap-build`
3. `implement-bytes`
4. `implement-error-result`
5. `implement-instrumentation`
6. `implement-coding`
7. `implement-text`
8. `implement-sql-value`
9. `implement-collation`
10. `implement-function-registry`
11. `implement-record-codec`
12. `implement-vfs-contracts`
13. `implement-posix-vfs`
14. `implement-page-cache`
15. `implement-read-pager`
16. `implement-btree-page`
17. `implement-btree-read`
18. `implement-storage-diagnostics`
19. `implement-lexer`
20. `implement-ast`
21. `implement-parser`
22. `implement-catalog-model`
23. `implement-catalog-loader`
24. `implement-bytecode`
25. `implement-read-vm`
26. `implement-binder`
27. `implement-logical-plan`
28. `implement-basic-optimizer`
29. `implement-read-lowering`
30. `implement-read-session-api`
31. `build-read-compatibility-harness`
32. `build-read-performance-baseline`
33. `implement-journal-contracts`
34. `implement-rollback-journal`
35. `implement-write-pager`
36. `implement-btree-write`
37. `implement-transaction-coordinator`
38. `implement-dml-ddl`
39. `build-writable-mvp-harness`
40. `establish-write-performance-baseline`
41. `implement-index-planning`
42. `implement-advanced-sql`
43. `implement-concurrency-locking`
44. `implement-wal`
45. `implement-c-api-compatibility`
46. `implement-extensions`
47. `harden-engine`

## Milestones

### Architecture milestone

Nodes 1 through 2 establish reviewed decisions, C++23 build policy, TDD tiers,
graph validation, and reproducible toolchain configuration.

### Storage-read milestone

Nodes 3 through 18 must open pinned-SQLite database files, decode records, read
B-tree pages, traverse rowid tables and indexes, and reject malformed data
without SQL execution.

The milestone includes an offline diagnostic command before any mutable
database operation is accepted.

### Read-only SQL milestone

Nodes 19 through 32 provide:

- A pure lexer, parser, and immutable AST.
- Catalog loading from `sqlite_schema`.
- Name and function binding.
- A basic physical planner.
- Typed bytecode and a read-only VM.
- A C++ prepare/step/finalize facade.
- Table scans, rowid lookup, simple predicates, projection, and deterministic
  result typing.
- Differential result and error tests against pinned SQLite.
- A matched read-performance baseline.

This milestone reads SQLite-created databases but does not modify them.

### Writable MVP milestone

Nodes 33 through 40 provide:

- Rollback-journal DELETE mode.
- Writable pager and B-tree mutation.
- Single-database, single-connection transactions and savepoints.
- Basic `CREATE TABLE`, `INSERT`, `UPDATE`, and `DELETE`.
- Implicit and explicit transactions.
- Deterministic crash recovery at every mutating I/O boundary.
- Modern-write/SQLite-read interoperability and `integrity_check`.
- A matched write-performance baseline.

The writable MVP intentionally excludes WAL, attached databases, triggers,
foreign keys, views, virtual tables, and advanced SQL.

### Compatibility expansion

Nodes 41 through 46 add index-aware planning, broader SQL semantics,
multi-connection locking, WAL, C API compatibility, and extensions. Each is a
separate reviewed scope, not part of the initial writable MVP.

### Hardening

Node 47 integrates:

- Debug and optimized builds.
- ASan, UBSan, and TSan where applicable.
- Differential, crash, fuzz, and malformed-input suites.
- Cross-compiler validation.
- Format and SQL compatibility matrices.
- Performance and allocation regression gates.
- Long-running recovery and concurrency campaigns.

Hardening does not replace the tests required by earlier nodes.

## Completion rule

A production node is complete only when:

- Its behavior was first expressed by a failing test.
- The expected failure was observed before implementation.
- The smallest correct implementation passes the focused test.
- The complete fast unit tier remains green.
- Malformed, boundary, ownership, and injected-failure cases are covered where
  applicable.
- Persistent encoders and decoders have independent SQLite golden vectors.
- Persistent transitions have deterministic crash tests.
- Performance-sensitive nodes have a pre-optimization baseline.
- The node does not introduce an upward dependency.
- Documentation and traceability are current.
- Design and code review are complete.

Documentation-only nodes require accuracy, consistency, source-reference, and
dependency-order review rather than an artificial failing unit test.

## Performance admission

An optimization may enter the roadmap only when it names:

- The deterministic workload and matched SQLite control.
- The measured gap or bottleneck.
- The native SQLite mechanism being considered.
- The invariant and ownership prerequisites for that mechanism.
- The expected effect.
- The correctness, crash, and non-target performance checks.

Profiler-instrumented timing is diagnostic evidence, not the claimed speedup.
If multiple native mechanisms depend on one another, they may be accepted as a
single parity cluster with one ADR and one final integrated measurement.
