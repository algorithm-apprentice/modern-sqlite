# ADR-0057: SQLite-Aligned Aggregate Roadmap

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0053 places aggregate functions, GROUP BY, HAVING, aggregate-local
DISTINCT, FILTER, and ordering after DISTINCT and compound SELECT.

The merged engine already provides:

- one SELECT query block with zero or one table source per core;
- WHERE, projection, DISTINCT, compounds, ORDER BY, LIMIT, and OFFSET;
- table, rowid, and ordinary-index access;
- a stable spill-capable sorter;
- bounded top-N storage;
- keyed ephemeral relations; and
- query-level metadata and public-session execution.

The parser preserves `f(DISTINCT x)`, `f(ALL x)`, `f(*)`, and empty argument
lists, but GROUP BY, HAVING, FILTER, aggregate-local ORDER BY, and every
aggregate call remain unsupported.

Aggregates cross too many boundaries for one implementation task:

- temporary storage needs a statement-wide memory and file budget before wide
  grouped, ordered, or DISTINCT aggregate queries are safe;
- aggregate function state has different lazy-allocation, error, collation,
  and finalization rules from scalar functions;
- GROUP BY and HAVING add clause-specific aliases, ordinals, rewrites, and
  representative-row behavior;
- existing indexes can change which expressions and errors are reached;
- aggregate state needs a new verified bytecode lifecycle;
- global and grouped lowering have different source and output schedules;
- DISTINCT, FILTER, and local ordering interact with MIN/MAX representative
  selection; and
- correctness-complete integration and performance evidence are substantial
  independent deliverables.

The previous all-in-one design draft mixed these concerns and required repeated
cross-layer review. This ADR therefore fixes only the SQLite compatibility
contract, architectural boundaries, dependency order, and completion
evidence. Each complex work package receives a focused child ADR immediately
before its production implementation.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `parse.y:652-661`, `parse.y:953-961`, and `parse.y:1241-1321`;
- `resolve.c:648-688`, `resolve.c:1155-1404`,
  `resolve.c:1637-1735`, and `resolve.c:1880-2144`;
- `expr.c:1200-1270`, `expr.c:2835-2890`,
  `expr.c:2940-3000`, and `expr.c:7490-7655`;
- `select.c:5475-5510`, `select.c:6588-7150`, and
  `select.c:8480-9050`;
- `func.c:1860-2331` and `func.c:3415-3482`;
- `vdbe.c:8000-8185`; and
- `pager.c:5048-5068`.

Modern prerequisites and constraints are in:

- ADR-0054: SQLite-Aligned ORDER BY and External Sorter;
- ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N;
- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT;
- `src/temporary_storage/temporary_storage.cpp`;
- `src/temporary_storage/ephemeral_relation.cpp`;
- `src/binder/bound_select.cpp`;
- `src/bytecode/program.cpp`; and
- `src/vm/vm.cpp`.

## Decision

### 1. Deliver aggregates as small ordered work packages

The aggregate slice is implemented in this fixed order:

| # | Work package | Scope |
|---|---|---|
| 1 | aggregate roadmap | This ADR and pinned behavior corpus |
| 2 | temporary budget core | Statement-owned retained-byte and live-file accounting, reservations, diagnostics, and deterministic pressure selection |
| 3 | shared relation arena | Memory-first keyed relations, one shared Pager/cache, root ownership, migration, poisoning, and reset |
| 4 | shared sorter arena | Shared run/file ownership, bounded fan-in and handles, pressure spill, reset, and existing ORDER BY migration |
| 5 | shared top-N ownership | Amend ADR-0055, move eager file-mode top-N roots into statement storage, and migrate compound consumers |
| 6 | aggregate syntax | GROUP BY, HAVING, FILTER, and aggregate-local ORDER BY syntax only |
| 7 | aggregate function state | Registry descriptors and COUNT, SUM, TOTAL, AVG, MIN, MAX, GROUP_CONCAT, and STRING_AGG state |
| 8 | aggregate binding | Clause legality, aliases, ordinals, deduplication, metadata, HAVING transfer, and representative analysis |
| 9 | aggregate planning | Global, indexed MIN/MAX, ordered-group, and sorted-group physical plans |
| 10 | aggregate bytecode and VM | Typed aggregate capability, verifier states, lazy state, step/final/reset/close |
| 11 | global aggregate execution | Global lowering, empty input, HAVING, lazy projection, ORDER elimination, reset |
| 12 | grouped execution | Ordered and sorted grouping, group boundaries, representative rows, HAVING |
| 13 | aggregate modifiers | DISTINCT, FILTER, local ordering, combined unique ordering, exact call schedule |
| 14 | integration | Session, differential, model, fuzz, metadata, OOM/allocation, memory/file, reset |
| 15 | performance evidence | Standalone immutable aggregate/grouping baseline |

Each package is one coherent sequential task and PR. A package does not begin
until its dependencies have merged. A child ADR is required before packages
2, 7, 8, 9, 10, 12, 13, and 15 because they introduce or materially change
an ownership, execution, or performance contract. Packages 2 through 5 share
one accepted temporary-storage child ADR but remain separate implementation
PRs.

### 2. Pin the accepted SQL surface

Ordinary SELECT cores gain:

```sql
SELECT [ALL | DISTINCT] result-column [, ...]
[FROM one-table [AS alias]]
[WHERE expression]
[GROUP BY expression [, ...]]
[HAVING expression]
```

Accept these aggregate forms:

```text
count()                  count(*)                 count(expr)
sum(expr)                total(expr)              avg(expr)
min(expr)                max(expr)
group_concat(expr)       group_concat(expr, separator)
string_agg(expr, separator)
```

Accept aggregate-local `DISTINCT`, `ALL`, ORDER BY, and
`FILTER (WHERE expression)` according to the pinned grammar.

This surface applies independently to simple SELECT arms of a compound.
VALUES does not become an aggregate core. Joins, subqueries, CTEs, views,
INSERT SELECT, windows, user-defined aggregate registration, percentile/JSON
aggregates, and schema/mutation features remain in ADR-0053 order.

### 3. Preserve SQLite clause resolution

For a singleton SELECT:

1. bind result expressions with aggregates allowed;
2. GROUP BY syntax or a result aggregate establishes aggregate-query status;
3. publish result aliases;
4. reject HAVING unless aggregate-query status already exists;
5. bind HAVING with aggregates allowed;
6. bind WHERE with aggregates disabled;
7. bind top-level ORDER BY with aggregates allowed only for an already
   aggregate query; and
8. bind GROUP BY with aggregates disabled.

Source columns precede aliases in GROUP BY and HAVING lookup.

GROUP BY uses ADR-0054's exact integer classifier: only recursively signed and
parenthesized integer forms whose unsigned literal magnitude is at most
`INT32_MAX` are ordinals. Recognized values outside `1..N` are errors. Larger
integer spellings are ordinary expressions.

Scalar FILTER reports `FILTER may not be used with non-aggregate ...`.
Nonempty scalar local ordering reports
`ORDER BY may not be used with non-aggregate ...` after function/arity and
ordinary-argument resolution but before resolving the local ordering terms.
Zero-argument local ordering is parsed and discarded before semantic
resolution.

Aggregate calls are structurally deduplicated per query core after binding.
Invocation order is first occurrence in result columns, then singleton
top-level ORDER BY, then HAVING.

Compound ORDER BY remains ADR-0056 output matching, not per-arm aggregate
binding. A scratch clone is resolved against each arm with ordinary errors
suppressed, compared with that arm's already bound outputs, and discarded.
Only OOM escapes speculative matching. A match becomes an output reference
and never changes an arm's aggregate status, invocation table, order, or
aggregate-term count.

### 4. Preserve aggregate result and error contracts

Core results are:

| Aggregate | Empty or no accepted value | Nonempty result |
|---|---|---|
| COUNT | integer `0` | integer count |
| SUM | NULL | integer until noninteger input, otherwise real |
| TOTAL | real `0.0` | real |
| AVG | NULL | real |
| MIN/MAX | NULL | owned winning value |
| GROUP_CONCAT/STRING_AGG | NULL | TEXT |

SUM uses checked signed-integer accumulation followed by
Kahan-Babuska-Neumaier floating accumulation. Integer overflow is latched while
stepping, may be cleared by a later noninteger input, and is reported only
during finalization.

GROUP_CONCAT/STRING_AGG omit NULL values. The default separator is `,`; a NULL
separator is empty. Size overflow and result-buffer allocation failure are
latched while later rows continue to execute, then reported during
finalization. Aggregate-state allocation and argument-conversion failures keep
their pinned immediate error paths.

MIN/MAX ignore NULL for their mathematical result, compare with the invocation
collation, retain the first comparator tie, and clone the winning storage
class.

Aggregate results have no declared type and no affinity. They do not inherit a
bare argument column's declared collation. The first explicit COLLATE in
ordinary argument subtrees may propagate to the result expression. Invocation
comparison collation remains separate from result-expression collation.

### 5. Preserve grouping and representative rows

GROUP BY comparison uses:

- NULL-equal semantics;
- integer/REAL numeric equivalence;
- no affinity;
- each term's resolved collation; and
- SQLite storage-class ordering otherwise.

Without an explicit ORDER BY, grouped row order remains unspecified even when
the initial sorter happens to produce key order.

SQLite permits bare result columns. Each group retains a representative source
snapshot:

- without MIN/MAX control, the first source row supplies it;
- a new strict MIN/MAX extremum replaces it;
- a comparator tie keeps it;
- NULL before any non-NULL MIN/MAX winner replaces it, so an all-NULL group
  retains the last source row;
- NULL after a winner keeps it;
- FILTER and DISTINCT can preserve either an initialized or stale
  representative gate depending on grouped/global and mixed invocation state;
  and
- the final MIN/MAX path reached for one source row controls snapshot
  replacement.

These edge schedules are pinned in the package-12 and package-13 child ADRs.
They are not inferred from generic "first row" behavior.

GROUP BY result reuse follows SQLite's selected-target rule:

- after ordinary resolution, one GROUP BY term records the rightmost
  structurally matching result column;
- only that selected target is eligible to reuse retained key storage;
- a simple representative source column/rowid target remains representative
  rather than becoming a key substitution; and
- other equal result expressions remain independently evaluated.

### 6. Preserve observable evaluation skipping

Skipped expression evaluation is correctness:

- LIMIT zero halts before source or aggregate work;
- unordered OFFSET may skip projection;
- bounded top-N evaluates keys/admission before non-key result payloads;
- global aggregates produce at most one candidate row, so top-level ORDER BY
  expressions are not evaluated merely for sorting;
- aggregate invocations discovered inside an ignored global ORDER BY still
  step and finalize;
- a GROUP BY/ORDER BY coalescing path may stream the first row before later
  group errors;
- an existing-index MIN/MAX path may stop before later source expressions;
  and
- a fully ordered GROUP BY access path may stop later groups after LIMIT.

GROUP BY/ORDER BY coalescing is allowed only for identical resolved term lists.
SQLite copies direction but not nondefault NULL placement. `ASC NULLS LAST`
and `DESC NULLS FIRST` therefore retain a separate outer ordering operation.
Outer ordering is removed only when the selected grouping path proves the
complete requested global order.

### 7. Preserve aggregate modifiers

FILTER executes before local ordering and arguments.

For accepted rows, local order keys execute before aggregate arguments.
MIN/MAX resolve but do not evaluate local ordering. Zero-argument ordering was
already discarded.

Ordinary DISTINCT aggregate membership:

- accepts exactly one argument;
- evaluates the argument before membership;
- uses NULL-equal, numeric-equivalent, collation-aware record equality without
  affinity; and
- retains the first distinct representative.

When one aggregate argument is structurally identical to the sole local order
key, SQLite evaluates it once. This reuse applies only to exactly one ordinary
argument and one key.

The exact DISTINCT-plus-identical-order case uses one unique ordered relation:

- every accepted row inserts with replace-existing semantics;
- the last equivalent encoded representative survives;
- no separate membership relation is opened; and
- finalization drains that relation in key order.

Other DISTINCT-plus-order cases retain the first distinct argument and its
associated first order key before insertion into an ordinary stable sorter.

Each invocation is processed end to end in invocation order. During
finalization, each invocation drains its own ordered input, if any, and
immediately finalizes before the next invocation.

### 8. Require statement-wide temporary resource bounds

Before aggregate consumers, the package-2 child ADR defines
`StatementTemporaryStorage`; packages 2 through 5 implement it in the split
order above. The completed prerequisite provides:

- one statement-wide retained-memory budget;
- allocation-sized accounting for sorter entries, relation records,
  top-N candidates, container capacity, and nodes;
- one bounded live-file budget;
- deterministic global spill selection;
- one shared relation spill arena with one bounded Pager cache;
- one bounded shared sorter-run arena; and
- shared top-N root ownership that amends ADR-0055's one-Pager/page-two
  assumptions.

Per-capability thresholds remain fairness triggers, not independent memory
budgets. Wide valid queries cannot reserve `capability_count × threshold`
memory or retain one spill file per invocation.

Any post-mutation failure that poisons the shared ephemeral Pager is
statement-wide. Execution stops, dependent capabilities close, and the whole
arena is destroyed. No unsupported savepoint-style recovery is promised.

Packages 2 through 5 must update existing ORDER BY, top-N, DISTINCT, and
compound consumers before aggregate implementation begins.

### 9. Keep architecture boundaries acyclic

The function registry gains immutable aggregate descriptors and move-only RAII
state. State creation is function-specific and lazy:

- COUNT, numeric aggregates, and MIN/MAX may require state for NULL input;
- GROUP_CONCAT/STRING_AGG skip NULL before state creation; and
- finalization without state uses an explicit empty-final callback.

Bound aggregate programs contain invocation IDs, group terms, retained source
leaves, representative leaves, HAVING, and result metadata. They contain no
register, sorter, relation, VM, or catalog pointer.

Logical plans add an aggregate phase before HAVING and projection.

Physical plans initially support:

- global streaming;
- existing-index MIN/MAX early-out;
- existing-path ordered grouping; and
- stable external sorted grouping.

Physical aggregate nodes publish at-most-one-row and delivered-order
properties so global ORDER elimination and GROUP BY/ORDER BY coalescing are
explicit.

Bytecode gains typed aggregate open, step, final, reset, and close operations.
The verifier checks declared descriptor and lifecycle consistency without
depending on `FunctionRegistry`. `Vm::Create` resolves aggregate symbols and
registered arity, matching scalar-call resolution. Halt/error/reset/finalize
cleanup owns open aggregate, sorter, relation, and top-N state uniformly.

The VM depends only on bytecode, runtime functions, temporary storage, and its
existing lower-layer execution context. It never receives binder, logical, or
physical plan objects.

### 10. Pin HAVING transfer narrowly

When GROUP BY is nonempty, eligible top-level AND leaves move from HAVING to
WHERE before aggregate analysis.

For every nonconstant visited subtree, there must be at least one matching
BINARY-collated GROUP BY term. Matching uses SQLite's dedicated comparison:
exact structure or a difference confined to the top-level COLLATE wrapper.
Constant-only subtrees need no GROUP BY match. Aggregate, always-false, nested
query, and outer-aggregate terms do not move.

This rewrite is observable because it changes source work and error order.
Global aggregates without GROUP BY never perform it.

### 11. Require package-local TDD, review, and evidence

Each package:

1. reads the applicable accepted ADRs and local pinned SQLite source;
2. writes one focused failing test for new behavior;
3. records the expected failure;
4. implements the smallest complete change;
5. runs the focused and complete fast unit tier;
6. adds malformed, boundary, ownership, OOM, allocation, and failure tests
   owned by that package;
7. receives one focused design/code review and one closure review at most; and
8. merges before the next package begins.

Routine build, test, format, clang-tidy, engineering-discipline, and graph
validation belong to the parent agent. Review agents only inspect design/code
and reference evidence; they do not rerun routine validation.

If review reveals that a package spans independent decisions, split the
package instead of starting another open-ended review cycle.

### 12. Require final compatibility and performance evidence

The completed slice must include:

- parser/binder clause and error matrices;
- aggregate state unit tests for empty, NULL, integer, real, text, blob,
  infinity, overflow, collation, separator, and value-limit behavior;
- representative-row probes for ordinary, all-NULL, FILTER, DISTINCT, and
  mixed MIN/MAX cases;
- lazy OFFSET, top-N, global ORDER elimination, group/order coalescing,
  indexed MIN/MAX, and ordered grouping call/error schedules;
- aggregate DISTINCT/order representative and single-evaluation probes;
- singleton and compound ORDER BY resolution;
- memory/file parity, statement-wide retained-byte and live-file ceilings,
  shared-arena poisoning, reset, and VFS failure cleanup;
- public metadata and reset/rebind behavior;
- deterministic model and grammar-aware fuzz coverage;
- exhaustive aggregate/storage/binder/planner/bytecode/lowering/session OOM
  coverage and exact allocation budgets; and
- a separate immutable aggregate/grouping performance baseline.

The performance contract leaves all existing baseline inputs unchanged. It
pins fixture, SQL, page/cache/temp-store settings, statement budget, thresholds,
result digest, source/group/step work, state calls, retained bytes, spills,
live files, and provenance. Every repetition exceeds five milliseconds. The
initial pre-optimization aggregate and paired-round wall/CPU guard is 40x
SQLite and may be tightened only through a new reviewed contract version.

## Explicit deferrals

This aggregate slice does not implement:

- joins or multi-source namespaces;
- scalar, EXISTS, IN, or FROM-clause subqueries;
- ordinary or recursive CTEs;
- views, INSERT SELECT, or CREATE TABLE AS SELECT;
- window use, OVER, inverse/value callbacks, or frames;
- percentile, median, JSON aggregates, user-defined aggregate registration, or
  WITHIN GROUP;
- hash aggregation;
- simple COUNT and DISTINCT-through-index optimizations beyond behavior
  required by the packages above;
- generated columns, constraints, foreign keys, triggers, or advanced DML;
  or
- general subroutine/coroutine bytecode.

Unsupported forms fail explicitly. No success-shaped stub, private unbounded
aggregate container, or upward dependency is accepted.

## Consequences

### Positive

- Each task has one primary layer and a bounded review surface.
- SQLite behavior is pinned before code without requiring one monolithic
  implementation design.
- Temporary-storage correctness is solved once for existing and future
  consumers.
- Aggregate state, binding, planning, VM execution, grouping, modifiers,
  integration, and performance can merge independently in dependency order.
- Review findings cause a task split rather than repeated whole-slice review.

### Negative

- Aggregate completion requires many sequential PRs.
- The temporary-storage prerequisite delays the first user-visible aggregate.
- Existing-index MIN/MAX and ordered grouping are required in the first
  correctness-complete implementation because skipped evaluation is
  observable.
- Representative-row and modifier schedules still require detailed child
  ADRs and extensive differential evidence.

## Rejected alternatives

### Keep the all-in-one aggregate design

Rejected because it mixed independent storage, runtime, binding, planning, VM,
execution, integration, and performance decisions and caused repeated
cross-layer review.

### Implement simple COUNT first while deferring architecture

Rejected because it would create a private execution path that later grouping
and aggregate state would need to replace.

### Use an unbounded map from group key to state

Rejected because group cardinality is database controlled and the shared
sorter is the pinned first mechanism.

### Give each aggregate modifier its own memory budget and file

Rejected because wide valid queries would multiply retained memory and file
descriptors by aggregate count.

### Require standard SQL grouped-column restrictions

Rejected because SQLite intentionally permits representative bare columns and
MIN/MAX coupling.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0015: SQL Values, Affinity, and Comparison
- ADR-0016: Collation Contracts and Built-ins
- ADR-0017: Scalar Function Registry
- ADR-0031: Typed Immutable Bytecode Programs
- ADR-0033: Immutable Read-Only SELECT Binding
- ADR-0034: Immutable Logical SELECT Plans
- ADR-0036: Physical Read Plan Lowering
- ADR-0037: RAII Read Session API
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N
- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT
