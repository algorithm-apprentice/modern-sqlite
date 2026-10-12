# ADR-0069: SQLite-Aligned Aggregate Modifier Execution

- Status: Accepted
- Date: 2026-10-12

## Context

ADR-0068 executes global and grouped aggregate plans but explicitly rejects
every invocation containing DISTINCT, FILTER, or aggregate-local ORDER BY.
Package 13 must now execute those already parsed, bound, deduplicated, and
planned modifiers without changing query-block scope, physical access choice,
outer ORDER BY, or public session ownership.

The merged engine provides:

- one immutable `BoundAggregateInvocation` per structurally deduplicated call,
  containing ordered arguments, one comparison collation, DISTINCT, local
  ordering terms, and an optional FILTER predicate;
- typed aggregate open, step, finalize, reset, and close bytecode;
- statement-owned spillable sorters and keyed relations sharing bounded memory
  and at most two temporary files;
- global and grouped representative snapshots controlled by MIN/MAX step
  effects;
- ordered and sorted grouping with exact group-boundary and output schedules;
  and
- exhaustive lowering and session allocation/OOM infrastructure.

Pinned SQLite 3.54.0 defines the modifier mechanism in:

- `src/select.c:6749-6835`, where DISTINCT and local-order ephemeral
  capabilities are opened or cleared for each accumulator lifecycle;
- `src/select.c:6837-6890`, where each invocation's ordered input is drained
  immediately before that invocation finalizes;
- `src/select.c:6897-7055`, where FILTER precedes local keys and arguments,
  DISTINCT membership precedes step/insertion, ordered inputs are deferred,
  and the representative-column gate is updated;
- `src/expr.c:7490-7655`, where aggregate argument, FILTER, and local-order
  expressions are coded; and
- `src/vdbe.c:7982-8185`, where aggregate step and final callbacks execute.

Local probes against the pinned checkout at
`cb547ab3e931c7766e24834af5ef6c4578863e3d` additionally establish:

- FILTER executes once per source row and false/NULL skips every local key and
  argument;
- accepted rows evaluate local keys before ordinary arguments;
- MIN/MAX resolve but never evaluate their local ordering expressions;
- exactly one argument structurally identical to exactly one local key is
  evaluated once;
- DISTINCT plus that exact identical key uses one ordered unique relation with
  replace-existing behavior, so the last numeric/collation-equivalent encoded
  value survives (`1.0` replaces `1`, and `a` replaces `A`);
- other DISTINCT-plus-order cases keep the first distinct argument together
  with its first evaluated ordering key;
- an unfiltered DISTINCT MIN/MAX duplicate preserves the prior representative
  gate: a stale replace captures a later duplicate while a stale keep does
  not;
- a filtered MIN/MAX skip supplies first-row replace/later-row keep in grouped
  execution and in global execution when no unfiltered MIN/MAX exists;
- when a global query has an unfiltered MIN/MAX, a later skipped filtered
  MIN/MAX leaves the previous invocation's gate unchanged;
- the final executed, filtered-skipped, or DISTINCT-skipped MIN/MAX path in
  invocation order controls the source-row snapshot; and
- ordered invocation finalization drains and finalizes one invocation fully
  before the next invocation begins its drain.

The implementation must preserve those schedules without adding unbounded
per-invocation containers, moving bound expressions into the VM, or inventing
new aggregate-function-specific runtime branches.

## Decision

### 1. Execute modifiers only inside the accepted aggregate statement boundary

Accept DISTINCT, FILTER, and aggregate-local ORDER BY for every global and
grouped statement shape accepted by ADR-0068:

- one ordinary non-DISTINCT SELECT core;
- global, indexed-MIN/MAX fallback, ordered-group, or sorted-group physical
  strategy;
- optional WHERE, HAVING, outer ORDER BY, LIMIT, and OFFSET; and
- every existing source access shape.

The strict indexed-MIN/MAX strategy itself remains modifier-free by ADR-0061;
a modifier-bearing MIN/MAX invocation uses ordinary global or grouped
execution.

Retain explicit rejection of:

- SELECT DISTINCT;
- compounds;
- multi-core aggregate execution;
- windows and user-defined aggregate registration; and
- every later advanced-SQL query-block shape.

Package 14 remains responsible for aggregate compounds/SELECT DISTINCT,
model/fuzz integration, and the final compatibility matrix.

Delivery update: ADR-0057 package 14 embeds this invocation lifecycle in
advanced SELECT cores, so SELECT DISTINCT and every supported compound operator
can consume global or grouped aggregate rows without a private aggregate path.

### 2. Add one shared aggregate-invocation lowering module

Create:

```text
src/lowering/
  aggregate_modifier_lowering.hpp
  aggregate_modifier_lowering.cpp
```

The module owns the task-cohesive per-invocation lifecycle:

- modifier strategy classification supplied by `PlanLowerer`;
- FILTER branching;
- local key and argument evaluation;
- DISTINCT membership;
- ordered record insertion;
- immediate aggregate step;
- ordered-input drain;
- per-invocation finalization;
- per-group reset; and
- final capability close.

ADR-0068's grouped module continues to own:

- group keys and boundaries;
- ordered versus sorted source scheduling;
- when a source row is offered to aggregate invocations;
- representative snapshot copying after all invocation paths; and
- completed-group HAVING/output/early-stop control.

ADR-0067's global access module continues to own cursor positioning and
indexed-MIN/MAX access. It delegates ordinary and strict indexed step calls to
the shared invocation module through `PlanLowerer`.

The shared module receives one synchronous borrowed input record containing
immutable bound metadata plus already allocated aggregate, relation, sorter,
register, constant, and symbol IDs. It owns no plan/runtime object and uses no
`std::function`.

### 3. Classify five internal invocation strategies

Use a new scoped enum whose new enumerators follow repository naming:

```cpp
enum class AggregateModifierStrategy : std::uint8_t {
  Direct,
  Distinct,
  Ordered,
  DistinctOrdered,
  UniqueOrdered,
};
```

Classification is deterministic:

- **Direct**: no DISTINCT and no effective local order;
- **Distinct**: DISTINCT with no effective local order;
- **Ordered**: local order without DISTINCT;
- **DistinctOrdered**: DISTINCT plus local order, except the exact combined
  case below; and
- **UniqueOrdered**: DISTINCT, exactly one argument, exactly one local key,
  and binder-published argument/key reuse.

For MIN/MAX, effective local order is always empty even though bound terms
remain part of invocation identity. MIN/MAX is therefore Direct or Distinct.

Append one derived field to `BoundAggregateInvocation`:

```cpp
bool argument_reuses_single_order_key = false;
```

The binder sets it only when there is exactly one ordinary argument, exactly
one local ordering term, and its canonical structural comparison succeeds.
That existing comparison already treats aliases transparently and canonicalizes
declared INTEGER PRIMARY KEY aliases with rowid. Lowering validates the
published shape and never rediscovers semantic equivalence.

### 4. Publish explicit modifier runtime records

For every invocation, lowering records:

- strategy;
- aggregate ID, contiguous argument block, and finalized result register;
- optional DISTINCT relation ID;
- optional ordered sorter ID;
- optional unique ordered relation ID;
- optional contiguous ordered-record register block;
- local key count and record field count;
- the binder-published shared key/argument flag; and
- whether the invocation is a MIN/MAX representative controller.

The record borrows no vectors. Statement-global vectors remain indexed by
`BoundAggregateInvocationId`, while the core's invocation list fixes execution
order.

Program limits continue to bound aggregate, sorter, relation, register,
constant, symbol, instruction, and owned-byte counts. No new public bytecode
resource kind is added.

### 5. Use three exact temporary record layouts

#### DISTINCT membership

Use one `EphemeralRelation` record:

```text
[argument]
```

It has one key field using the invocation comparison collation, ascending
direction, and default NULL-first placement. Equality is NULL-equal,
numeric-equivalent, collation-aware, and affinity-free through the canonical
record comparator.

#### Ordinary ordered input

Use one stable `RecordSorter` record:

```text
[local order keys]
[ordinary arguments]
```

Key metadata comes from bound local ordering terms. Equal keys retain source
insertion order through ADR-0054's stable sorter. If there is exactly one
argument structurally equal to the sole key, store one field:

```text
[shared key/argument]
```

The drain reads that field as the argument.

#### Unique ordered DISTINCT

Use one `EphemeralRelation` record:

```text
[shared key/argument]
```

Its key metadata is the sole local ordering term. Every accepted source row
uses replace-existing insertion. Iteration therefore supplies DISTINCT
ordering while retaining the last equivalent encoded representative.

`DistinctOrdered` opens both the one-field membership relation and the ordinary
ordered sorter. It preserves the first distinct argument and the local keys
evaluated for that same first row.

### 6. Preserve FILTER, key, argument, and membership order

Process each invocation end to end in bound invocation order.

For one source row:

1. prepare the representative gate only when this filtered MIN/MAX requires a
   row-default gate;
2. evaluate FILTER;
3. on false or NULL, skip the entire invocation;
4. for effective local ordering, evaluate keys left to right;
5. evaluate ordinary arguments left to right, except the exact shared
   key/argument expression is evaluated once;
6. perform DISTINCT membership/insertion when present;
7. insert ordered input or invoke aggregate step immediately; and
8. continue to the next invocation.

Consequences:

- rejected FILTER rows evaluate no key, argument, membership operation, or
  step;
- `DistinctOrdered` evaluates keys before its argument even when the argument
  is a duplicate;
- a duplicate skips sorter insertion and immediate step;
- Direct and Distinct step during source traversal;
- Ordered, DistinctOrdered, and UniqueOrdered defer step until finalization;
  and
- MIN/MAX never evaluates stored local-order expressions.

Every expression, conversion, relation, sorter, or immediate step failure
propagates at its exact source-row phase.

### 7. Preserve DISTINCT representatives

Direct DISTINCT insertion uses keep-existing mode. A duplicate branches around
aggregate step without changing its argument representative.

`DistinctOrdered` also uses keep-existing membership before sorter insertion.
The first distinct argument and the local keys from that same row survive.

`UniqueOrdered` uses replace-existing mode and joins inserted/replaced control
flow immediately after insertion. Equivalent keys replace the complete one
field record, so the last encoded storage-class/text representative survives.

NULL is one distinct value. Integer/REAL equivalents share one membership key.
Text equivalence uses the invocation or ordering collation required by the
selected strategy.

### 8. Preserve the representative gate across skipped MIN/MAX paths

One persistent effect register remains shared by every MIN/MAX invocation in
bound invocation order. Initialize it to Replace before source processing so
verified duplicate branches on a first candidate have a defined state.

An executed MIN/MAX step overwrites the register with its returned effect.

An unfiltered DISTINCT duplicate does not modify it. The prior gate is
therefore intentionally stale:

- stale Replace captures the duplicate source row; and
- stale Keep retains the prior representative.

Before a filtered MIN/MAX predicate:

- grouped first-row paths load Replace;
- grouped subsequent-row paths load Keep;
- global paths load first-row Replace/later-row Keep only when the core has no
  unfiltered MIN/MAX controller; and
- a global core containing any unfiltered MIN/MAX leaves the current gate
  untouched before a filtered invocation.

If FILTER rejects or DISTINCT later rejects, that prepared/stale gate remains.
If step executes, its effect replaces the gate.

After every invocation path for the source row, ADR-0068 copies representative
leaves only when the final gate is Replace. Non-MIN/MAX invocations never
modify the gate. Ordered MIN/MAX is effective Direct/Distinct and therefore
still participates at source-row time.

Global default-gate paths own one seen-source-row register updated after every
accepted source row, irrespective of FILTER or DISTINCT acceptance.

### 9. Drain and finalize one invocation completely

At global completion or a group boundary, process invocations in bound order:

1. for Ordered or DistinctOrdered, rewind the sorter;
2. read each argument payload and step the aggregate in stable key order;
3. for UniqueOrdered, rewind the relation, read its shared field, and step in
   relation key order;
4. finalize that invocation immediately into its result register; and
5. continue with the next invocation.

An empty ordered capability performs no step and still runs ordinary empty
finalization.

Ordered drain steps never request representative effects because MIN/MAX local
order is ignored and every remaining ordered aggregate has no representative
preference.

Delayed ordered-step and final errors stop before later invocation drains,
matching SQLite's per-invocation finalization order.

### 10. Reset and close modifier capabilities with aggregate state

After a completed grouped row when another group will execute:

1. reset every finalized aggregate slot;
2. reset each open DISTINCT/ordered capability to Writing in invocation order;
3. reset the representative gate state required by the next group's first
   source row; and
4. step the new group's first row.

Global execution never resets between source rows.

At final completion or early stop:

- close every open relation/sorter capability;
- close every finalized aggregate slot; and
- preserve existing outer sorter/top-N and source-cursor cleanup.

VM error and public statement reset continue to destroy every capability and
the complete statement temporary-storage arena. The next execution opens fresh
relations, sorters, and aggregate states while retaining bindings.

### 11. Keep resource ownership statement-wide

Every modifier capability is created by the existing
`StatementTemporaryStorage`.

- all DISTINCT relations share one relation arena and Pager cache;
- all ordered sorters share one extent-managed PMA file;
- group sorters, outer ORDER sorters/top-N relations, and invocation modifiers
  compete under one retained-byte budget;
- at most the existing relation and sorter files are live; and
- one poisoned arena aborts the complete statement.

Capabilities remain lazy at execution time. `LIMIT 0` opens none.

No per-invocation memory budget, file, Pager, raw owning pointer, or private
container is introduced.

### 12. Verify exact behavior and failure phases

Development follows red-green-refactor.

Focused tests cover:

- FILTER false/NULL/true and filter-before-key/argument call order;
- MIN/MAX local-order non-evaluation;
- key-before-argument order for accepted rows;
- exact single evaluation for one identical argument/key;
- ordered COUNT, SUM/TOTAL/AVG, GROUP_CONCAT, and STRING_AGG results;
- stable equal-key order, ASC/DESC, NULL placement, and collations;
- DISTINCT NULL, numeric equivalence, storage-class ownership, collations, and
  first representative;
- UniqueOrdered last equivalent representative and direction;
- DistinctOrdered first argument/first key association;
- global and grouped ordinary, filtered, DISTINCT-stale, all-NULL, tie, and
  multiple-MIN/MAX representative matrices;
- empty input, all-filtered input, duplicate-only later rows, multiple groups,
  HAVING, outer ordering, OFFSET/LIMIT, and reset/rebind;
- memory/file parity, many simultaneous modifiers, spill, shared-arena
  poisoning, and VFS failure cleanup;
- exact successful lowering/session allocation budgets;
- exhaustive lowering/session OOM cuts;
- public-session and pinned-SQLite differential cases; and
- unchanged unmodified global/grouped and nonaggregate schedules.

The package also runs the complete Debug, Release, ASan/UBSan, GCC, production
Clang-Tidy, fuzz-build, formatting, engineering-discipline, graph, whitespace,
and review gates.

Package 15 retains ownership of the standalone aggregate/grouping performance
baseline. Package 13 makes no unmeasured speed claim.

## Explicit deferrals

This package does not implement:

- SELECT DISTINCT or compound aggregate execution;
- windows, inverse/value callbacks, frames, or OVER;
- user-defined aggregate registration;
- percentile, median, or JSON aggregates;
- hash aggregation;
- DISTINCT-through-index or simple COUNT optimizations;
- joins, subqueries, CTEs, or views; or
- the final model/fuzz/performance integration packages.

Unsupported statement shapes continue to return typed errors.

## Consequences

### Positive

- All accepted aggregate modifier combinations reuse one bounded invocation
  mechanism.
- FILTER, DISTINCT, and local ordering preserve SQLite's observable evaluation
  and error phases.
- Representative snapshots retain the difficult filtered/skipped/stale gate
  behavior rather than approximating MIN/MAX.
- Existing sorter/relation bytecode and statement-wide storage need no new VM
  opcode or runtime container.
- Global and grouped execution share one invocation lifecycle.

### Negative

- Wide aggregate programs may own one or two logical capabilities per
  invocation, though physical files and memory remain statement bounded.
- Representative correctness requires an intentionally persistent gate whose
  stale value is observable after DISTINCT skips.
- Ordered aggregate step errors move to group/global finalization by design.
- The first implementation does not exploit index-provided DISTINCT or local
  aggregate order.

## Rejected alternatives

### Evaluate FILTER after keys or arguments

Rejected because SQLite skips all local key and argument work on rejected
rows.

### Step ordered aggregates during source traversal

Rejected because argument order changes results, overflow behavior, delayed
errors, and concatenation output.

### Use one relation per DISTINCT and one sorter per ORDER in every case

Rejected because the exact identical DISTINCT/order case is one replace-
existing ordered relation with last-representative behavior.

### Deduplicate before evaluating local order keys

Rejected because SQLite evaluates keys before the argument and membership
check. Duplicate rows may still expose key callbacks or errors.

### Reset the representative gate at every source row

Rejected because unfiltered DISTINCT MIN/MAX duplicates intentionally preserve
the previous gate, and that stale state changes bare-column representatives.

### Give each modifier its own temporary-storage budget or file

Rejected because database-controlled invocation counts would multiply memory
and file handles. ADR-0058's statement-wide arenas are the accepted ownership
boundary.

### Add modifier-specific aggregate VM opcodes

Rejected because existing aggregate, relation, sorter, expression, branch, and
copy instructions express the complete lifecycle.

### Rediscover argument/key equivalence in lowering

Rejected because binding already owns canonical expression identity, aliases,
rowid-alias normalization, and invocation deduplication. Publishing one derived
boolean avoids a third structural-comparison implementation.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0031: Typed Immutable Bytecode Programs
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
- ADR-0068: SQLite-Aligned Grouped Aggregate Execution
