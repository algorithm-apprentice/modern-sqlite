# ADR-0008: SQLite Performance Parity Strategy

- Status: Proposed
- Date: 2026-10-03

## Context

SQLite contains decades of algorithmic, ownership, memory-layout, and
filesystem optimizations. A cleaner C++ implementation can become
substantially slower through avoidable allocation, copying, validation,
virtual dispatch, ownership, cache, or planner costs.

The sibling Modern LevelDB project demonstrated that:

- A pinned native implementation is the strongest baseline.
- Matched mechanisms are required for fair comparisons.
- Fixed-work counters and profiles explain gaps better than isolated symbols.
- Interdependent native mechanisms may need to land as one parity cluster.
- Validation belongs at explicit boundaries; trusted internal paths should not
  repeatedly revalidate established invariants.

## Decision

Performance is a first-class acceptance dimension.

### Reference and provenance

- Use the pinned SQLite 3.54.0 source as the primary reference.
- Record source identity, compile options, compiler, flags, executable digest,
  platform, and dirty state in every benchmark artifact.
- Never silently substitute a different SQLite build.

### Comparable configurations

Every comparison records and, for matched-mode results, aligns:

- Page size and reserved bytes.
- Cache size and mmap policy.
- Journal mode and synchronous setting.
- Temp storage and auto-vacuum settings.
- Schema, indexes, statistics, data corpus, insertion order, and SQL.
- Prepared-statement reuse, bindings, and transaction boundaries.
- Thread count and connection count.

Product-default comparisons and matched-mechanism comparisons are reported
separately.

### Measurement rules

- Generate deterministic corpora and query orders before timing.
- Separate database creation, prepare, warmup, execution, and verification.
- Verify rows, value types, errors, and final database integrity outside timed
  loops.
- Report wall time and process CPU time.
- Add fixed-work diagnostics for allocations, bytes copied, VFS calls, pages
  read/written, cache outcomes, B-tree comparisons, VM instructions, and
  planner work.
- Keep profiling hooks in an optional development-only instrumentation leaf.
- Do not use instrumented timings as the claimed speedup.
- Preserve failed and noisy runs; do not cherry-pick replacements.

### Initial workloads

The first single-threaded family includes:

- Prepare-only.
- Reused prepared statement.
- Rowid hit and miss.
- Indexed hit and miss.
- Full scan.
- Predicate scan.
- Batched insert.
- Batched update and delete.
- Commit and rollback.

Each runs in cache-fit and cache-pressure sizes. WAL, concurrent, and
long-running mixed workloads are added only after those features exist.

### Optimization admission

Before changing a hot path:

1. Name the deterministic workload and matched SQLite control.
2. Establish a repeatable gap.
3. Identify a concrete mechanism through counters, allocation evidence, or a
   profile.
4. Audit the native SQLite mechanism and its prerequisites.
5. Record the decision in an ADR.
6. Measure the complete integrated path.
7. Re-run correctness, crash, fuzz, and non-target performance gates.

If several SQLite mechanisms depend on one another, the project may implement
them as one reviewed parity cluster instead of requiring each isolated part to
show a local speedup.

Early CI uses a severe-regression guard and report-shape validation, not a
noisy claim of exact parity. Stable workload-specific gates are introduced
only after reproducible baselines exist.

## Consequences

- Performance-sensitive nodes require baselines before optimization.
- Stronger safety or durability behavior must be named and measured rather
  than used to excuse all overhead.
- Benchmarks remain correctness checks as well as timing tools.
- The project can adopt SQLite's proven mechanisms without copying its source
  architecture.
