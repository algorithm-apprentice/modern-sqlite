# Implementation Dependency DAG

## Purpose

Modern SQLite is implemented as a directed acyclic graph of independently
verifiable nodes. Dependency edges are technical prerequisites, not permission
to develop multiple nodes in parallel.

Only one node is developed at a time. Independent ready nodes remain pending
until the current node is complete and reviewed.

An oversized node normally uses one coherent slice per ordered pull request.
An accepted delivery ADR may batch two through four related coherent slice
commits with per-commit evidence and review. Named larger exceptions require
an accepted ADR; ADR-0106 authorizes the seven-commit node-43 prerequisite
closure. A related batch may open as a draft after its first accepted commit;
later accepted commits update that same draft. Integrated gates run against
the final exact batch head, and changing an earlier commit invalidates its
dependent suffix. The next DAG node remains blocked until every slice of the
active node is complete.

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
41. `optimize-one-pass-mutations`
42. `implement-index-planning`
43. `implement-advanced-sql`
44. `implement-concurrency-locking`
45. `implement-wal`
46. `implement-c-api-compatibility`
47. `implement-extensions`
48. `harden-engine`

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

Nodes 33 through 41 provide:

- Rollback-journal DELETE mode.
- Writable pager and B-tree mutation.
- Single-database, single-connection transactions and savepoints.
- Basic `CREATE TABLE`, `INSERT`, `UPDATE`, and `DELETE`.
- Implicit and explicit transactions.
- Deterministic crash recovery at every mutating I/O boundary.
- Modern-write/SQLite-read interoperability and `integrity_check`.
- A matched write-performance baseline.
- SQLite-aligned one-pass stable-rowid UPDATE and DELETE scan mutation that
  remediates the admitted baseline guard failures.

The writable MVP intentionally excludes WAL, attached databases, triggers,
foreign keys, views, virtual tables, and advanced SQL.

### Compatibility expansion

Nodes 42 through 47 add index-aware planning, broader SQL semantics,
multi-connection locking, WAL, C API compatibility, and extensions. Each is a
separate reviewed scope, not part of the initial writable MVP.

`implement-advanced-sql` is delivered as ordered vertical slices under
ADR-0053. The shared spill-capable sorter and specialized bounded ordering
relation arrive with ORDER BY. The general keyed membership/materialization
relation arrives with DISTINCT and compounds, then these capabilities are
reused by grouping, automatic join indexes, recursive CTEs, and windows.
Multi-source and joined-output scopes precede joins. Verified subroutines
precede scalar and EXISTS queries; coroutine prerequisites remain attached to
streaming derived-query, view, and CTE consumers. Joined stored covering-index
equality access reuses the positioned-source producer under ADR-0085;
ADR-0086 extends the same producer with noncovering equality access and
table-backed outer replay. ADR-0087 integrates parameterized ranges through
the same producer and canonical traversal emission. ADR-0088 defines the
bounded canonical record-prefix comparison prerequisite for shared
automatic-index integration. ADR-0089 defines that integration through
configured-backend admission, canonical lookup relations, and borrowed
source cursors. ADR-0090 defines source correlation through the same query
owners and producers, with lazy inline emission inside one lexical bytecode
region and separate binding/value dependencies. Source-free queries retain
the verified value-call interface. ADR-0091 defines the next bounded outer
expression-alias admission through immutable targets and the same capture
contract. ADR-0092 defines query-containing result-reference instances
through canonical template binding, delivered as ordered binder-foundation,
expression-copy, and aggregate-ordering slices; outer aggregate ownership
remains an ordered follow-up. ADR-0096 corrects arithmetic joined-rowid scan
amplification through the same positioned producer and canonical ON readiness,
without relaxing the unchanged model deadline. The
[implemented-engine reference audit](sqlite-reference-audit.md) retains
prerequisite corrections before further feature expansion. ADR-0095 repairs
exact journal metadata reads without changing tolerated page-record crash
tails. ADR-0097 corrects the production POSIX nonempty-regular-file existence
policy through the canonical VFS query, preserving custom-backend recovery
semantics. ADR-0098 retires session-owned pending completion after canonical
automatic rollback without discarding valid durable-cleanup retries.
ADR-0094 corrects nullable upper-only range traversal through the same
borrowed index boundaries and existing key blocks in every range consumer.
ADR-0099 corrects generated wildcard publication across preserving USING
boundaries through the canonical joined namespace without changing explicit
physical qualification.
ADR-0100 corrects function numeric typing through the canonical SQL-value
parsers while preserving distinct affinity, CAST, and arithmetic behavior.
ADR-0101 contains expected codec/page/read-cursor ownership and diagnostic
allocation failures at their existing typed boundaries without changing
borrowed decoding or persistent behavior.
ADR-0102 defines change-aware local verification and bounded fast-local unit
execution while retaining isolated qualification and required hosted gates.
ADR-0103 corrects the writable payload reader's consumed overflow-link
boundary while retaining strict whole-file diagnostics.
ADR-0104 defines one conditional branch emitter for all current
predicate contexts while preserving scalar value-expression semantics and
existing optimizer placement.
ADR-0105 adds explicit single-source rowid range access through the
existing table cursor and typed range contracts.
ADR-0106 keeps one coherent commit per slice, places all seven
remaining node-43 prerequisite corrections in one reviewed pull request, and
uses smaller related batches for later feature work.
ADR-0107 assigns each joined ON conjunct one pre-open or source-ready
phase, consumes selected access terms once, and bounds deterministic scalar
rowid probes without changing join order or stored/automatic-index key
domains.
Persistent constraints,
generated values, foreign keys, views, schema operations, and
trigger programs retain conflict-specific statement/transaction outcomes and
add their own crash evidence. No later compatibility-expansion node begins
until every advanced-SQL slice and its integrated review are complete.

### Hardening

Node 48 integrates:

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
