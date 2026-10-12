# ADR-0070: Matched Aggregate and Grouping Performance Baseline

- Status: Accepted
- Date: 2026-10-12

## Context

ADR-0057 packages 1 through 14 are merged. Global aggregation, indexed MIN/MAX,
ordered and sorted grouping, HAVING, aggregate modifiers, SELECT DISTINCT,
compounds, and statement-wide temporary ownership are executable. Package 15
must record their first standalone, pre-optimization performance evidence.

The existing performance infrastructure already owns:

- pinned SQLite identity and semantic build configuration;
- immutable workload, result, fixture, and work contracts;
- prepare/warmup/measurement/verification separation;
- paired-round wall and process-CPU timing;
- diagnostic-only allocations, VFS/cache/VM counters, and SQL probe tags;
- a strict baseline runner with provenance and bounded child processes; and
- the deterministic ORDER BY fixture and its regeneration contract.

`benchmarks/distinct_compound_performance.cpp` reuses the canonical ORDER BY
C++ runner, and `tools/distinct_compound_performance.py` scopes the canonical
Python runner to an independent contract. Package 15 follows that prior art;
it does not create another timing loop, digest, fixture generator, or process
controller.

The current diagnostic counters do not expose aggregate lifecycle or
statement-wide temporary peaks. Reading those values through the public
session API would add benchmark policy to production ownership. A separate
metrics container would duplicate the instrumentation leaf.

## Decision

### 1. Deliver one independent aggregate contract

Add:

```text
benchmarks/aggregate_performance.cpp
tools/aggregate_performance.py
tests/performance/aggregate-workloads-v1.json
tests/performance/aggregate_performance_tool_test.py
tests/performance/aggregate_benchmark_cli_test.py
benchmarks/aggregate-baseline-v1/
```

The aggregate timing and diagnostic binaries reuse the existing C++ execution,
digest, configuration, verification, and report machinery. The aggregate
runner reuses the existing strict JSON, baseline/provenance, bounded-child,
aggregation, and exact-ratio machinery.

The read, write, index, ORDER BY, and DISTINCT/compound workload manifests,
fixture SQL, expected work, and committed baseline artifacts remain unchanged.
There is no aggregate optimization in this package.

Existing baseline input hashes also pin the legacy harness and runner source
files. Their anonymous-namespace probe registration and fixed target lookup
cannot be extended without invalidating that evidence. Aggregate-specific
boundary adapters may therefore register native probes, extend diagnostic
reports, and select the aggregate timing target while still delegating timing,
digest, execution, JSON, process, and provenance operations to the unchanged
shared mechanisms. This exception adds no new engine algorithm or compatibility
layer and does not authorize copying the shared execution infrastructure.

### 2. Match the existing immutable fixture and engine settings

Reuse `tests/fixtures/order_by_performance/ordered.db` and its independently
pinned SQL, digest, page count, schema, statistics, and 65,536-row corpus:

- page size 4096 and cache size 512 pages;
- mmap disabled;
- rollback journal DELETE and synchronous FULL;
- query-only, single-threaded execution;
- the same `items` table and `items_score_desc` index;
- per-case memory or file temporary storage; and
- per-case retained-memory threshold, with the existing merge fan-in of 16
  and statement-wide two-file ceiling.

The threshold is also the existing statement retained-memory budget, not a
new per-aggregate allowance. File-mode merge working sets retain ADR-0058's
documented exception and are reported separately from ordinary admission.

Do not regenerate or replace the fixture merely to improve a measurement.
Fixture integrity and complete source contents are verified before and after
each child execution outside the timed region.

### 3. Pin a representative mechanism matrix

The first contract contains these independent cases:

| Case | Required mechanism |
|---|---|
| global-functions-memory | COUNT, SUM, TOTAL, AVG, MIN, and MAX over a complete source stream |
| global-empty-memory | Empty finalization without source steps |
| indexed-min-memory | Existing-index MIN early out |
| indexed-max-memory | Existing-index MAX early out |
| ordered-group-memory | Index-delivered score groups and satisfied output order |
| sorted-group-memory | Expression grouping with the stable group sorter |
| sorted-group-file | The same sorted grouping under a bounded spilling budget |
| high-card-group-memory | One group per category and complete owned result output |
| grouped-having-topn-memory | Completed groups, HAVING, and outer LIMIT/OFFSET ordering |
| distinct-state-memory | Independent COUNT/SUM DISTINCT membership lifecycles |
| filtered-state-memory | Disjoint FILTER partitions and argument skipping |
| local-order-memory | Ordered concatenation input and delayed steps |
| unique-local-order-memory | Identical DISTINCT argument/order-key unique relation |
| wide-modifier-file | Concurrent modifier consumers sharing both bounded arenas |
| aggregate-compound-memory | Grouped and global arms feeding existing ordered consumers |

Every SQL string, measured iteration count, result row/byte count, typed result
digest, source/group/invocation work count, and diagnostic schedule is pinned
independently. Empty and indexed early-out cases repeat enough fixed work to
exceed the five-millisecond floor; iteration counts are fixed before recording
the baseline.

Expected output comes from the pinned SQLite oracle and an independent fixture
model, never from Modern's timed execution. Each warmup, measured, diagnostic,
smoke, and verification object is an independent immutable contract value.
Validators reject missing fields, wrong types, unknown cases, changed
configuration, unsupported work counts, and altered digests.

The timing SQL is fixed before implementation:

```sql
-- global-functions-memory
SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0)
FROM items;
-- global-empty-memory
SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0)
FROM items WHERE 0;
-- indexed-min-memory / indexed-max-memory
SELECT min(score) FROM items;
SELECT max(score) FROM items;
-- ordered-group-memory
SELECT score,count(id) FROM items GROUP BY score ORDER BY score;
-- sorted-group-memory / sorted-group-file
SELECT score+0,count(id) FROM items GROUP BY score+0 ORDER BY 1;
-- high-card-group-memory
SELECT category,count(id) FROM items GROUP BY category ORDER BY category;
-- grouped-having-topn-memory
SELECT score+0 AS g,count(id) AS n,sum(id) AS s
FROM items GROUP BY score+0
HAVING count(id)>8 ORDER BY s DESC,g LIMIT 64 OFFSET 64;
-- distinct-state-memory
SELECT count(DISTINCT score),sum(DISTINCT score) FROM items;
-- filtered-state-memory
SELECT count(id) FILTER (WHERE flag=1),sum(score) FILTER (WHERE flag=0)
FROM items;
-- local-order-memory
SELECT group_concat(score ORDER BY score DESC,id) FROM items;
-- unique-local-order-memory
SELECT group_concat(DISTINCT score ORDER BY score) FROM items;
-- wide-modifier-file
SELECT group_concat(score ORDER BY id),group_concat(score ORDER BY id DESC),
       count(DISTINCT category),group_concat(DISTINCT score ORDER BY score)
FROM items;
-- aggregate-compound-memory
SELECT score+0 AS g,count(id) FROM items GROUP BY score+0
UNION ALL SELECT -1,count(id) FROM items
ORDER BY 1 LIMIT 64 OFFSET 64;
```

Memory cases retain a 64 MiB threshold; the two file cases retain 2 MiB.
Empty and indexed MIN/MAX measurement phases execute 65,536 independent
reset/reuse iterations, with one iteration for warmup, diagnostic, and smoke.
Other measurement phases execute one complete fixture workload. Their fixed
floor is verified rather than assumed from Modern's slower execution.

### 4. Preserve the physical mechanisms during diagnostics

The existing nondeterministic `modern_sqlite_probe(tag,value)` identity
function supplies engine-independent expression call evidence.

Do not wrap GROUP BY terms, indexed MIN/MAX arguments, WHERE predicates, or
selected access keys in probes when doing so would change the admitted
physical mechanism. Place probes in ordinary aggregate arguments or completed
result expressions instead. Validate each tag's exact count independently.
Diagnostic SQL must preserve the timing statement's result and mechanism.

Both engines report their existing VM/cache/allocation/spill counters. Modern
additionally reports actual successful aggregate open, step, finalize, reset,
and close operations plus statement retained-byte and live-file peaks.
SQLite's expression probes and native statement counters remain explicitly
different evidence; do not mislabel inferred native state work as observed
function callbacks.

Pin the observed native mechanism counters independently: indexed MIN/MAX and
empty input have zero full-scan and sort operations; complete single-source
cases have 65,535 full-scan steps; the two-arm aggregate compound has 131,070.
Ordered grouping has no sort, expression/high-cardinality grouping has one,
and HAVING/top-N plus the ordered compound have two. Indexed executions are
bounded to 20 VM operations and four cache hits in each engine, in addition to
Modern's exact one aggregate step.

Native `SQLITE_DBSTATUS_TEMPBUF_SPILL` is zero for these admitted cases,
including file mode. This is pinned evidence about SQLite's temporary-buffer
counter, not a claim that it measures all sorter-file writes. Modern's two file
pressure cases independently require live file acquisition and positive spill
PMA-record writes. Pager page-write counters do not count PMA journal writes
and are not substituted for that evidence. Ordered/plain/empty/indexed cases retain no temporary relation or
sorter memory; sorted/modifier/compound cases must report a positive retained
peak. Validators reject mechanism drift rather than accepting matching result
rows alone.

### 5. Extend the existing probe collector, not the production API

Use one canonical `ProbeCounterCollection`.

Add a narrowly scoped aggregate-work collection mode and named internal metric
tags in a reserved range outside SQL expression tags. Extend the same
collection with additive operation recording and maximum-value recording.
Existing row-probe scopes remain the default and do not collect internal
aggregate/temporary metrics.

The VM records successful typed aggregate lifecycle operations through the
instrumentation leaf. Statement temporary storage records peaks at its
existing accounting and file-lease transitions. File creation and spill
classification retain the current VFS/native SQLite evidence rather than
being synthesized from thresholds.

Every new hook is removed by the existing instrumentation-disabled compilation
contract. Ordinary timing binaries use the uninstrumented production library.
The diagnostic binary alone enables the aggregate collection mode. Existing
diagnostic schemas and unused-tag checks retain their current behavior.

Tests first demonstrate mode isolation, scoped restoration, exact operations,
and peak recording. No session, VM public API, storage ownership, allocation
algorithm, format, parser, planner, or SQL behavior changes for diagnostics.

### 6. Record a complete, non-cherry-picked baseline

Use three paired rounds with canonical/reverse case order and alternating
Modern/SQLite engine order. Every timing child contains three repetitions
whose wall time exceeds five milliseconds. Report both wall and process CPU.
Instrumented runs are fixed-work evidence, not timing comparisons.

Each admitted case must satisfy the ADR-0057 40x aggregate and every-paired-round
wall/CPU severe-regression guard. Record actual failures and noise; never
reduce the work, drop a hard case, increase the guard, or replace a failed
round silently.

An observed, repeatable guard failure may justify a separately reviewed
SQLite-mechanism optimization after this baseline supplies evidence. It does
not authorize speculative pre-optimization tuning inside the contract work.

Keep baseline binaries, compiler/flags, SQLite hashes, source state, host,
command schedule, raw child output, configuration, and work provenance.
Committed baseline validation is mandatory in CTest and CI. A missing baseline
fails explicitly, as with the current DISTINCT/compound contract.

The existing provenance runner requires a clean source tree. Review and validate
the source/bootstrap contract before committing it, then configure the timing
binaries from that reviewed commit and record the baseline. The source and
baseline commits remain one sequential reviewable package and are squash-merged
only when the final evidence and all required gates pass. Unrelated local
diagnostics are not benchmark inputs and are preserved outside Git's source
status; they are never deleted to satisfy this gate.

### 7. Close package 15 before the next SQL slice

Before merging:

- review this design and the implementation;
- observe focused red tests before new diagnostic/tool behavior;
- verify fixture/oracle/model agreement and all benchmark modes;
- validate every recorded baseline artifact and the 40x guard;
- preserve existing performance contracts and ordinary-library isolation; and
- run the required fast, compiler, sanitizer, fuzz, formatting, discipline,
  dependency-graph, and review gates.

The advanced-SQL node remains in progress. Multi-source binding and joined
output follow only after this aggregate package is reviewed and merged.

## Consequences

- Aggregate bottlenecks are measured before optimization rather than guessed.
- Ordered access, grouping, modifiers, and spill compete under the same
  accepted temporary ownership.
- Diagnostic changes reuse one instrumentation collector and remain absent
  from ordinary production binaries.
- Existing benchmark inputs and evidence stay immutable.
- An actual admitted performance gap may require another coherent optimization
  PR before the aggregate performance package can close.

## References

- ADR-0008: SQLite Performance Parity Strategy
- ADR-0039: Pinned Read Performance Baseline
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0061: SQLite-Aligned Aggregate Planning
- ADR-0068: SQLite-Aligned Grouped Aggregate Execution
- ADR-0069: SQLite-Aligned Aggregate Modifier Execution
