# ADR-0068: SQLite-Aligned Grouped Aggregate Execution

- Status: Accepted
- Date: 2026-10-12

## Context

ADR-0063 makes the ungrouped aggregate subset executable but defers
representative bare columns together with grouped execution. ADR-0061 already
publishes `kOrderedGroup` and `kSortedGroup` physical strategies, grouping
delivery metadata, exact GROUP BY/ORDER BY coalescing, and all supported access
paths. Package 12 must now complete representative snapshots and grouped
queries without pulling aggregate DISTINCT, FILTER, aggregate-local ordering,
compounds, or the final integration matrix into the same change.

The merged engine provides:

- immutable bound aggregate programs with ordered invocations, grouping terms,
  source leaves, representative leaves, HAVING, and selected result matches;
- physical Empty, SingleRow, table, rowid, covering-index, and noncovering-index
  inputs in forward or reverse order;
- stable statement-owned external sorters and bounded top-N relations;
- typed aggregate open, step, finalize, reset, and close bytecode;
- MIN/MAX representative effects from every successful aggregate step;
- global aggregate descriptor, register, expression, and output lowering; and
- shared statement-wide temporary-storage memory and file budgets.

Pinned SQLite 3.54.0 defines the grouped schedule in:

- `src/select.c:8587-8710`, where grouped state, the grouping sorter, key
  registers, result and reset subroutines, and ordered-versus-sorted input are
  established;
- `src/select.c:8711-8784`, where unsorted source rows store grouping keys plus
  retained aggregate columns in one stable sorter;
- `src/select.c:8786-8842`, where current and previous grouping keys are
  compared, the completed group is output before the next group is reset and
  stepped, and the final group is emitted;
- `src/select.c:8850-8877`, where empty groups are suppressed, aggregate
  functions finalize, HAVING runs, and the result/ORDER/LIMIT pipeline executes;
- `src/select.c:6900-7055`, where aggregate invocations run in order and the
  final reached MIN/MAX path controls representative-column replacement; and
- `src/select.c:6749-6785`, where every aggregate state is reset between
  groups.

Local probes against the pinned checkout at
`cb547ab3e931c7766e24834af5ef6c4578863e3d` additionally establish:

- a stable sorted grouping path evaluates every source grouping key before it
  starts aggregate arguments, while an ordered path steps arguments as rows
  arrive;
- `LIMIT 0` evaluates no grouping key, aggregate argument, HAVING, ORDER BY, or
  result expression;
- an ordered/coalesced `LIMIT` stops before aggregate arguments from later
  groups, whereas sorted grouping still materializes every source key before
  draining only the required groups;
- a selected result expression identical to a GROUP BY term is evaluated only
  during source grouping and is reused from retained key storage at output;
- without MIN/MAX control the first row supplies bare columns;
- strict MIN/MAX replacement selects its source row, a tie keeps the prior
  row, and an all-NULL group retains the last row;
- with multiple MIN/MAX invocations, the final MIN/MAX reached in bound
  invocation order controls replacement for that source row; and
- an unsatisfied ORDER BY with syntactic LIMIT evaluates order keys before
  non-key result payload and skips payload for a rejected top-N candidate.

The implementation must preserve those schedules while retaining Modern
SQLite's acyclic architecture. It must not execute physical plans directly in
the VM, place bound objects in bytecode, add a private unbounded grouping map,
or weaken statement-wide temporary-storage ownership.

## Decision

### 1. Execute one complete grouped subset

Accept one ordinary, non-DISTINCT SELECT core with:

- a nonempty `BoundAggregateProgram::group_by`;
- physical strategy `kOrderedGroup` or `kSortedGroup`;
- zero or more ordinary aggregate invocations;
- optional WHERE, HAVING, LIMIT, OFFSET, and top-level ORDER BY;
- every currently supported aggregate function and scalar expression;
- bare source-column and rowid references through representative snapshots;
- Empty, SingleRow, table-scan, rowid-lookup, covering-index, and noncovering
  index inputs; and
- either an access/group-sort order that completely satisfies top-level order
  or the existing external-sort/runtime-top-N output path.

Reject explicitly:

- SELECT DISTINCT;
- compounds;
- aggregate DISTINCT, FILTER, or aggregate-local ORDER BY;
- malformed physical grouping metadata; and
- every multi-core or deferred query-block shape.

Package 13 remains the sole owner of modifier scheduling. The later aggregate
integration package remains responsible for compounds, SELECT DISTINCT,
complete model/fuzz matrices, and the standalone aggregate performance
contract.

### 2. Complete global representative snapshots

The ADR-0063 global and indexed-MIN/MAX plans additionally accept
representative source-column and rowid leaves reachable from result or HAVING
expressions.

The global schedule reuses the grouped representative rules:

- without MIN/MAX control, the first stepped source row supplies the snapshot;
- every MIN/MAX step may replace it;
- the final MIN/MAX invocation reached for one source row controls conflicting
  effects;
- ties keep the prior snapshot;
- all-NULL MIN/MAX input retains the last stepped row;
- indexed MIN/MAX captures the exact early-out or NULL-fallback candidate; and
- empty input leaves representative registers NULL while ordinary aggregate
  empty-final values remain unchanged.

Top-level ORDER-only representative leaves remain ignored, as ADR-0063
requires. Global ORDER BY expressions remain unevaluated for the single
candidate row.

### 3. Add one internal grouped-lowering module

Create:

```text
src/lowering/
  grouped_aggregate_lowering.hpp
  grouped_aggregate_lowering.cpp
```

The module owns one cohesive grouped-execution responsibility:

- ordered and sorted group input schedules;
- group-sorter spool and drain;
- current-versus-previous key comparison;
- group-boundary detection;
- aggregate open, step, finalize, reset, and close sequencing;
- representative snapshot replacement;
- completed-group HAVING, output, and early-stop control; and
- verifier-compatible normalization of cursor, sorter, top-N, and aggregate
  capability states.

`PlanLowerer` retains:

- physical-chain inspection;
- descriptor, cursor, symbol, constant, and register allocation;
- the existing Empty, SingleRow, table, rowid, covering-index, and noncovering
  index traversal and cursor-state control;
- general scalar and aggregate-result expression lowering;
- existing outer ORDER BY/top-N candidate and drain operations;
- shared index-key expression and key-block construction; and
- all global aggregate, nonaggregate, compound, DISTINCT, and mutation paths.

The module receives one synchronous borrowed input record containing immutable
binding and plan metadata plus already allocated bytecode identities. A narrow
private services interface delegates only the retained general operations.
The existing access emitters call two grouped task hooks:

- consume one source row after selected access and residual filters; and
- complete grouped input from the current access state.

All query-kind selection remains centralized in those hooks. Individual table,
rowid, and index emitters do not acquire grouped scheduling branches. Neither
the record nor interface owns data or uses `std::function`.

This does not generalize ADR-0067's global access emitter. The two modules have
different completion and cursor-join invariants and remain separate.

### 4. Reuse existing bytecode and temporary capabilities

Grouped execution adds no public bytecode instruction, VM capability, runtime
container, or public API type.

Each bound invocation retains:

- one aggregate descriptor and capability slot;
- one contiguous argument block; and
- one finalized-result register.

Grouped layout additionally allocates only the resources required by the
selected plan:

- current and previous grouping-key register blocks;
- a has-group flag and comparison scratch;
- representative registers keyed by retained source identity;
- one shared MIN/MAX effect register and named replacement-effect constant
  when representative leaves and controlling MIN/MAX invocations coexist;
- for sorted grouping, one group-sorter descriptor, record block, and current
  retained-source register map;
- existing outer sorter or top-N records when top-level order is not
  satisfied; and
- existing LIMIT/OFFSET, access-key, and noncovering-rowid registers.

Grouping-key equality uses existing `CompareInstruction` operations with
`SqlComparison::kIs`, `TypeAffinity::kNone`, and each term's resolved
collation. This supplies NULL-equal, exact numeric-equivalent, no-affinity
group comparison without adding record-comparison bytecode or a hash table.

### 5. Preserve one explicit source-value context

Grouped expression emission distinguishes three compile-time source views:

```cpp
enum class GroupedExpressionSource : std::uint8_t {
  AccessRow,
  SortedRow,
  Representative,
};
```

- `AccessRow` reads the current table/index candidate through existing cursor
  mappings.
- `SortedRow` reads source identities retained in the current group-sorter
  record.
- `Representative` reads the completed group's snapshot.

The source view is lowering state only. It does not enter bytecode or the VM.
Aggregate-call expressions continue to copy finalized result registers.

Retained leaf maps use bound source identity:

- one canonical rowid identity for `BoundRowIdExpression` and a declared
  INTEGER PRIMARY KEY alias; and
- `BoundSourceColumnId` for every other source column.

They do not key storage by expression occurrence, because multiple expression
nodes may reference the same source value.

### 6. Materialize sorted grouping with one stable record

`kSortedGroup` uses one `RecordSorter`.

Its record layout is:

```text
[all GROUP BY key values in bound term order]
[unique retained source identities]
```

The payload identity order is:

1. `source_leaves` in binder order; then
2. previously unseen `representative_leaves` in binder order.

Every retained leaf must be a bound source column or rowid. Duplicate
identities occupy one field.

The sorter descriptor:

- uses every grouping term as a key;
- uses the term's resolved collation;
- uses the direction and default NULL placement published for that exact
  `group_term_index`; and
- retains the stable equal-key insertion order guaranteed by ADR-0054.

For every source row that passes guards, selected access, and residual filters:

1. evaluate GROUP BY expressions in bound order from `AccessRow`;
2. evaluate and retain source identities without evaluating aggregate
   arguments;
3. insert one complete sorter record; and
4. continue until source exhaustion.

After source cursors are normalized closed:

1. rewind the group sorter;
2. if empty, close it without opening or finalizing aggregate state;
3. read grouping keys and retained identities into their current blocks;
4. consume rows through the common grouping loop; and
5. close the group sorter before final outer-order draining or halt.

This preserves SQLite's phase split: all source grouping work precedes every
aggregate argument and representative update on the sorted path.

### 7. Stream ordered grouping directly from the selected access

`kOrderedGroup` consumes the selected physical input without a group sorter.

For every accepted candidate:

1. evaluate GROUP BY expressions in bound order from `AccessRow`;
2. compare them with the retained previous-key block when a group is active;
3. when the key changed, complete the previous group before resetting any
   aggregate or stepping the current row;
4. copy the current key block into the previous-key block for a new group;
5. step the current row; and
6. advance according to the physical scan direction.

The grouping comparison always uses bound GROUP BY term order. The physical
`grouping_order` permutation proves adjacency and optional final ordering; it
does not change SQL grouping-key identity or result-term mapping.

Empty input emits no group. SingleRow and exact lookup inputs create one group
only when their residual filters accept the candidate.

### 8. Use one common group lifecycle

After LIMIT-zero bypass, aggregate capabilities open before source processing.
The bytecode verifier therefore sees one stable Accumulating state across
filtered candidates and runtime first-group detection.

The lifecycle is:

```text
initialization:
  Open every invocation

first accepted row:
  Step row

same group:
  Step row

group boundary:
  Finalize every invocation
  evaluate HAVING and candidate output
  if continuing, Reset every invocation
  Step first row of next group

source exhaustion:
  if a group is active, Finalize and output it
  otherwise Finalize the unstepped empty states without output
  Close every invocation
```

Invocation order is always `BoundAggregateProgram::invocations`.
Arguments execute left to right within one invocation, and invocations execute
end to end in program order.

The current row is never stepped before the previous group has completed.
Errors from previous-group finalization, HAVING, ordering, or projection
therefore occur before any aggregate argument from the next group.

SQLite suppresses its result subroutine entirely for empty grouped input.
Modern's typed aggregate verifier does not permit a successful halt or close
from Accumulating state because stepped states may contain delayed errors.
Package 12 therefore finalizes unstepped built-in states and discards their
results. Empty finalization evaluates no SQL expression, performs no aggregate
step, invokes no user-defined callback because registration is deferred, and
produces no row.

### 9. Preserve representative-row replacement exactly

Representative registers exist only for
`BoundAggregateProgram::representative_leaves`.

Without any MIN/MAX invocation:

- the first stepped row of each group copies all representative leaves; and
- later rows keep that snapshot.

With one or more MIN/MAX invocations:

- every MIN/MAX step writes its effect to one shared register;
- later MIN/MAX invocations overwrite earlier effects for the same row;
- after every invocation has stepped, copy representative leaves only when
  the final effect is `BytecodeAggregateStepEffect::kReplaceRepresentative`;
  and
- ties, worse values, and NULL after a non-NULL winner keep the snapshot.

The aggregate-state contract already returns replacement for every NULL before
a non-NULL winner. An all-NULL group therefore copies every source row and
retains the last row. The final MIN/MAX invocation in bound invocation order
controls conflicting effects.

Representative leaves are copied in binder order after all successful steps
for the row. A step failure stops immediately and exposes no partial result.

Package 13 may refine how FILTER and DISTINCT skip or preserve the shared
effect register. Package 12 does not pre-implement those modifiers.

### 10. Reuse only the selected GROUP BY result target

For each GROUP BY term whose `result_column` is present, lowering may map that
selected result expression to the retained previous-key register.

The rules are:

- consider GROUP BY terms in bound order;
- the first term selecting one result column establishes its key register;
- later terms selecting the same result column do not overwrite it;
- a direct source-column or rowid result remains representative and is not
  replaced by a key register; and
- every other structurally equal result expression is evaluated independently
  from the representative snapshot.

Alias references to a key-backed selected result follow the same retained
register. Representative-context expression emission consults this selected
key map before traversing the expression payload, so projection and outer
ordering share the same retained value. This preserves one evaluation of the
chosen grouping expression while retaining SQLite's bare-column and MIN/MAX
coupling.

### 11. Preserve HAVING, ORDER BY, OFFSET, and LIMIT phases

Every completed group runs:

1. aggregate finalization;
2. HAVING, with false or NULL suppressing the candidate;
3. either direct/coalesced output or the existing outer-order candidate path;
4. OFFSET/LIMIT at the phase represented by the physical plan; and
5. reset only when another group will be processed.

When no top-level order exists, or when
`PhysicalSortNode::input_order_satisfied` is true:

- allocate no outer sorter or top-N capability;
- do not reevaluate ORDER-only expressions;
- apply OFFSET after HAVING and before visible projection;
- evaluate visible result columns left to right only for an accepted row; and
- after emitting a positive LIMIT's final row, stop before resetting or
  stepping a later group.

For ordered grouping, reaching a new key may evaluate that key before the
previous group exhausts LIMIT, but no aggregate argument from the new group
executes after the limit is satisfied.

When top-level order is not satisfied:

- use the existing external-sort/runtime-top-N selection;
- evaluate HAVING before ordering work;
- without syntactic LIMIT, preserve payload-then-key evaluation;
- with syntactic LIMIT, evaluate keys and top-N admission before non-key
  payload;
- insert every admitted completed group;
- process all groups before outer-order drain; and
- apply OFFSET and LIMIT while reconstructing mapped output fields.

`LIMIT 0` completes before opening a cursor, sorter, top-N relation, or
aggregate capability and before evaluating OFFSET, guards, grouping keys, or
SQL expressions.

### 12. Normalize capability states and preserve reset behavior

Existing access lowering keeps its distinct closed, unpositioned, positioned,
and exhausted paths. Each path invokes grouped completion without merging
incompatible cursor states; explicit close or halt-owned cleanup then preserves
the verified state. Covering and noncovering index paths retain their existing
live cursor sets.

Early ordered-group LIMIT completion relies on halt cleanup for live source
cursors and any outer temporary capability. Sorted grouping reaches an
exhausted or unpositioned source state before rewinding the group sorter;
existing cursors retain no page pin across sorter work and remain owned until
halt/error cleanup. Final outer-order drain starts only after the group sorter
and aggregate capabilities are closed.

Any expression, aggregate, sorter, cursor, I/O, conversion, or allocation
failure propagates unchanged through VM-owned cleanup. There is no
success-shaped fallback from ordered to sorted grouping in lowering.

Statement reset destroys aggregate and temporary-storage runtime state,
retains parameter bindings, and reruns the immutable program from closed
capabilities.

### 13. Verify behavior, ownership, and bounded resources

Development follows red-green-refactor.

Focused lowering and execution tests cover:

- explicit acceptance of ordered and sorted grouped physical plans;
- Empty, SingleRow, table, rowid, covering-index, and noncovering-index input;
- forward/reverse ordered grouping, equality prefixes, ranges, and grouping
  term permutations;
- stable sorted grouping in memory and after spill;
- NULL, integer/REAL numeric equivalence, storage classes, and collations;
- ordinary first-row representatives;
- MIN/MAX strict replacement, ties, all-NULL groups, mixed aggregates, and
  conflicting multiple-MIN/MAX invocation order;
- equivalent global representative schedules, including indexed MIN/MAX;
- direct representative targets versus selected key-backed expressions;
- HAVING true, false, NULL, aggregate references, representative references,
  and errors;
- LIMIT-zero bypass, OFFSET projection skipping, ordered early stop, sorted
  key materialization, and outer top-N payload skipping;
- satisfied and unsatisfied ORDER BY, default and nondefault NULL placement,
  and two simultaneous sorter capabilities;
- finalization/reset/close order and statement reset/rebind;
- exact allocation budgets and exhaustive lowering/session OOM cuts; and
- unchanged global aggregate and nonaggregate schedules.

The package also runs:

- the complete fast unit tier;
- public-session and pinned-SQLite differential grouped cases;
- statement temporary-storage memory/file and injected-failure coverage;
- Debug, Release, ASan/UBSan, GCC, and production Clang-Tidy builds;
- formatting, engineering-discipline, dependency-graph, and whitespace
  checks; and
- focused design/code review plus one closure review at most.

No artificial performance claim is made in package 12. Package 15 owns the
immutable aggregate/grouping baseline and optimization admission.

## Explicit deferrals

This package does not implement:

- aggregate DISTINCT;
- FILTER;
- aggregate-local ORDER BY;
- SELECT DISTINCT or compound aggregate execution;
- hash aggregation;
- simple COUNT or DISTINCT-through-index optimizations;
- windows, subqueries, CTEs, views, or joins; or
- the final aggregate model, grammar-aware fuzz, and performance corpus.

Unsupported shapes return typed lowering errors.

## Consequences

### Positive

- Grouped execution reuses the existing typed aggregate and temporary-storage
  contracts without adding VM surface.
- Ordered access preserves early LIMIT and error skipping.
- Sorted grouping preserves source-key/aggregate phase separation and stable
  representative behavior.
- Representative-row and selected-key semantics are explicit rather than
  inferred from ordinary projection.
- The grouped state machine is isolated from the global access emitter while
  reusing the already reviewed physical access traversal.

### Negative

- Existing access emitters gain one centralized accepted-row/completion
  consumer seam that must preserve every nonaggregate schedule.
- Sorted grouping may keep a group sorter and an outer sorter/top-N capability
  open in the same statement.
- Retained source identities add sorter payload fields beyond the grouping
  keys.
- The first implementation prioritizes exact schedules over register reuse or
  specialized grouping performance.

## Rejected alternatives

### Generalize the global aggregate access emitter

Rejected because global completion has one accumulator and one candidate row,
while grouped execution owns repeated final/reset transitions, representative
snapshots, group-key state, and early-stop boundaries.

### Copy the global or ordinary access emitters

Rejected because the physical cursor traversal, range, lookup, and residual
rules are already shared and reviewed. Duplicating them would create another
large control-flow implementation and invite drift.

### Reuse ordinary source-row OFFSET/LIMIT/projection through mode flags

Rejected because those phases differ from grouped HAVING and completed-group
output. Reuse stops at accepted-row and source-completion hooks rather than
scattering grouped branches through each access emitter.

### Add group-specific VM opcodes

Rejected because existing typed aggregate, sorter, comparison, copy, branch,
and cursor instructions already express the complete schedule.

### Store complete projected rows in the group sorter

Rejected because aggregate arguments and representative expressions have
different evaluation phases. The sorter retains only grouping keys and source
identities needed to reconstruct those later phases.

### Use a hash table of aggregate states

Rejected because cardinality is database-controlled, state payloads are
move-only, spill ownership is already solved by the stable sorter, and the
first mechanism is pinned to SQLite's sorted grouping.

### Add an abandon-unstepped aggregate opcode

Rejected because package 12 uses only built-in aggregate descriptors whose
empty finalization is already defined and non-observable for grouped empty
input. A new VM lifecycle operation solely to avoid discarded empty-final
values would expand public bytecode without a current SQL requirement.

### Snapshot every source column

Rejected because the binder already publishes exact source and representative
leaf sets. Retaining unrelated columns would multiply sorter payload and
register ownership without a current requirement.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0036: Physical Read Plan Lowering
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0059: SQLite-Aligned Aggregate Function State
- ADR-0060: SQLite-Aligned Aggregate Binding
- ADR-0061: SQLite-Aligned Aggregate Planning
- ADR-0062: Typed Aggregate Bytecode and VM State
- ADR-0063: SQLite-Aligned Global Aggregate Execution
- ADR-0067: Global Aggregate Access Lowering Module
