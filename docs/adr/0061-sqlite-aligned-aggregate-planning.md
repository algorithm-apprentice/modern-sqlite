# ADR-0061: SQLite-Aligned Aggregate Planning

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0060 publishes immutable aggregate programs but deliberately stops logical
planning with `aggregate planning is not implemented`. Package 9 must now make
aggregate relational phases and physical strategies inspectable without
adding bytecode, VM state, grouping execution, or SQL-visible aggregate
results.

The current planning pipeline already provides:

- immutable per-core logical source, WHERE, projection, DISTINCT, compound,
  ORDER BY, and LIMIT nodes;
- table, rowid, and ordinary-index physical access descriptors;
- stable external-sort and bounded top-N plans;
- deterministic index costs, covering analysis, and residual predicates; and
- explicit planner-to-lowering ownership.

Pinned SQLite 3.54.0 defines the relevant mechanisms in:

- `src/select.c:5475-5528`, where a single ungrouped MIN/MAX invocation
  creates an ordering request;
- `src/select.c:8494-8519`, where identical GROUP BY and ORDER BY lists are
  recorded for possible coalescing;
- `src/select.c:8587-8885`, where grouped input either arrives in a grouping
  order or is materialized through a stable sorter;
- `src/select.c:8889-9030`, where global aggregates stream input and indexed
  MIN/MAX may stop after the first qualifying row;
- `src/where.c:5494-5528`, where an access path may group rows without proving
  the exact requested output order; and
- `src/where.c:6200-6260`, where complete ordering, grouping-only ordering,
  scan reversal, and MIN/MAX early-out properties are published.

Pinned probes additionally establish:

- global aggregation produces at most one candidate row;
- a single plain MIN/MAX invocation may use a matching rowid or ordinary-index
  order and stop after the first qualifying non-NULL row;
- HAVING, multiple aggregate invocations, FILTER, and a collation mismatch
  prevent that early-out;
- an ordered noncovering index may still be selected for MIN/MAX or grouping;
- an index prefix may group the same resolved terms in a different order,
  such as index `(a,b)` for `GROUP BY b,a`;
- such a grouping-only order does not prove `ORDER BY b,a`;
- a stable group sorter can also satisfy an identical default-NULL-placement
  ORDER BY by adopting its directions;
- `ASC NULLS LAST` and `DESC NULLS FIRST` retain a separate outer order; and
- a global aggregate does not evaluate top-level ORDER BY expressions merely
  to sort its single candidate row, although aggregate invocations discovered
  there still step and finalize.

## Decision

### 1. Add explicit logical aggregate and HAVING phases

Append these payloads and kinds to the logical-plan variants:

```cpp
struct LogicalAggregateNode {
  LogicalNodeId input;
  std::size_t core_index = 0;
};

struct LogicalHavingNode {
  LogicalNodeId input;
  std::size_t core_index = 0;
  BoundExpressionId predicate;
};
```

An aggregate core contributes these semantic phases:

```text
Source
  -> optional WHERE Filter
  -> Aggregate
  -> optional HAVING
```

Their canonical postorder placement depends on the statement shape:

```text
one-core unordered non-DISTINCT:
  Source -> WHERE? -> Aggregate -> HAVING? -> LIMIT? -> Projection

one-core ordered non-DISTINCT:
  Source -> WHERE? -> Aggregate -> HAVING? -> Order -> LIMIT? -> Output

DISTINCT or compound arm:
  Source -> WHERE? -> Aggregate -> HAVING? -> Projection -> DISTINCT?
```

Compound, statement ORDER BY, LIMIT, and final output nodes remain above the
completed arm roots. The one-core shapes preserve lazy projection: OFFSET
rows do not evaluate result expressions, and an Order node owns key/admission
and payload evaluation instead of consuming an eager Projection.

`LogicalAggregateNode` refers to the retained core's
`BoundAggregateProgram`; it copies no invocation, grouping, leaf, expression,
or result metadata. `LogicalHavingNode` is distinct from the source WHERE
filter so later lowering cannot evaluate aggregate predicates against source
rows.

The new variant alternatives and enum values are appended, preserving all
existing numeric kind values.

### 2. Publish aggregate physical strategy and delivered grouping order

Add:

```cpp
enum class PhysicalScanDirection : std::uint8_t {
  kForward,
  kReverse,
};

enum class PhysicalAggregateStrategy : std::uint8_t {
  kGlobal,
  kIndexedMinMax,
  kOrderedGroup,
  kSortedGroup,
};

enum class PhysicalMinMaxNullStrategy : std::uint8_t {
  kNone,
  kNonNullFirstWithSingleNullFallback,
};

struct PhysicalMinMaxPlan {
  BoundAggregateInvocationId invocation;
  PhysicalMinMaxNullStrategy null_strategy =
      PhysicalMinMaxNullStrategy::kNone;
  bool argument_fixed_by_equality = false;
};

struct PhysicalGroupingOrderTerm {
  std::uint32_t group_term_index = 0;
  SortOrder order = SortOrder::kAscending;
  BoundNullPlacement null_placement = BoundNullPlacement::kFirst;
};

struct PhysicalAggregateNode {
  PhysicalNodeId input;
  std::size_t core_index = 0;
  PhysicalAggregateStrategy strategy = PhysicalAggregateStrategy::kGlobal;
  std::optional<PhysicalMinMaxPlan> min_max{};
  std::vector<PhysicalGroupingOrderTerm> grouping_order{};
  bool at_most_one_row = false;
};

struct PhysicalHavingNode {
  PhysicalNodeId input;
  std::size_t core_index = 0;
  BoundExpressionId predicate;
};
```

`PhysicalTableScanNode` and `PhysicalIndexScanNode` gain a scan direction,
defaulting to forward. Existing ordinary plans remain forward-only in this
package. Aggregate planning may select reverse traversal for MIN/MAX or to
prove a complete grouping output order.

For an ordinary index term:

```text
effective_desc =
    (catalog_term.order == DESC) XOR (scan_direction == REVERSE)
```

The delivered direction is descending exactly when `effective_desc` is true.
Its delivered default NULL placement is NULLS LAST when descending and NULLS
FIRST when ascending. Reverse range execution selects the opposite physical
start/end roles and cursor-advance operation while retaining the logical
lower/upper predicates in the immutable plan.

`PhysicalSortNode` retains its existing `input_order_satisfied` property.
`PhysicalAdvancedOrderNode` gains the same property. A satisfied order node
remains in the immutable plan as the statement-order contract, but later
lowering must emit no sorter and must not evaluate ordinary order keys merely
for sorting.

The new physical payloads and kinds are appended so existing numeric kind
values remain stable.

### 3. Preserve ordinary access analysis per aggregate core

The existing source, conjunction, rowid, and index candidate analysis is
factored into a per-core helper. It accepts:

- the core source and WHERE predicate;
- the source values required after access; and
- whether an otherwise legal noncovering full index scan may compete because
  aggregate order avoids a sorter or enables MIN/MAX early-out.

For aggregate cores, required source values are exactly:

- residual WHERE leaves;
- `BoundAggregateProgram::source_leaves`; and
- `BoundAggregateProgram::representative_leaves`.

This avoids descending through aggregate-call result references and keeps
covering decisions aligned with the binder's retained dataflow contract.

Selected equality and range predicates keep the existing one-time key
evaluation and residual ordering rules. Filtering a correctly ordered access
path preserves its grouping or MIN/MAX order.

For one-core plans, the existing candidate and selected-index accessors remain
populated. Multi-core plans retain their current empty statement-level
candidate span; each selected access path is still inspectable through its
physical leaf and aggregate node.

### 4. Select global streaming by default

An aggregate program with no GROUP BY uses `kGlobal` unless the strict
MIN/MAX rule below succeeds.

Global aggregation:

- consumes the selected source stream without materializing groups;
- publishes `at_most_one_row=true`;
- has no grouping-order terms; and
- keeps `min_max` empty.

Empty input still reaches aggregate finalization in later packages. A
physical Empty source is therefore not the final statement result; it is only
an empty input stream to the global aggregate node.

### 5. Admit only a strict indexed MIN/MAX early-out

`kIndexedMinMax` requires all of:

- no GROUP BY and no HAVING;
- exactly one unique invocation in the core;
- a MIN or MAX descriptor with one ordinary argument;
- no DISTINCT, FILTER, or aggregate-local ordering;
- an argument that resolves, through alias and top-level COLLATE wrappers, to
  the source rowid/INTEGER PRIMARY KEY alias or one source column; and
- an access path where that source identity is either fixed by a compatible
  selected equality or is the next ordered field after the selected equality
  prefix with the invocation collation.

For a rowid argument, an ordinary rowid-table or schema-table scan supplies
the order. For a column argument, the path must use a supported, nonpartial,
ordered ordinary index. A range on the MIN/MAX field is permitted.

An equality fixes the argument only when it constrains the same source
identity and its indexed affinity/collation semantics imply one equivalence
class under the aggregate invocation comparator. Ordinary `= NULL` retains
its existing reject-NULL empty-input path. `IS NULL` admits only the NULL
class and uses the NULL fallback protocol below.

The chosen scan direction places the requested extremum first:

- MIN follows an ascending physical term and reverses a descending term;
- MAX reverses an ascending term and follows a descending term.

The aggregate node records a `PhysicalMinMaxPlan` for the sole invocation.
Later lowering uses a non-NULL-first protocol:

1. visit candidate non-NULL values in extremum order without evaluating the
   skipped NULL region;
2. stop after the first row that passes residual predicates and steps the
   invocation; and
3. only when no qualifying non-NULL row exists, visit the NULL region and
   process exactly one qualifying NULL row through the ordinary aggregate
   step and representative-row capture.

This preserves the accepted all-NULL and pre-winner representative schedule.
Rowid arguments use `kNone` because rowid is never NULL. Nullable column
arguments use `kNonNullFirstWithSingleNullFallback`. An equality-fixed
non-NULL argument sets `argument_fixed_by_equality=true` and may stop after
the first residual-qualifying row without an ordered varying field. A
noncovering index may perform table lookup before residuals and representative
capture.

If multiple paths qualify, the existing deterministic access preference is
applied among qualifying paths. Any qualifying early-out path is preferred to
a non-early-out global scan. Collation mismatch or any failed precondition
falls back to `kGlobal`.

### 6. Prefer an existing ordered grouping path when proved

For nonempty GROUP BY, `kOrderedGroup` is selected when the chosen input path
guarantees that equal group keys are contiguous.

The proof may use:

- Empty, SingleRow, or one-row rowid lookup input;
- rowid-table or schema-table order for one rowid-equivalent varying group
  term; or
- a supported ordered index after its equality-constrained prefix.

A group term is fixed only when one of these proofs succeeds:

- its complete bound expression is source-independent and deterministic, so
  it is statement-constant; or
- a selected equality constrains the same direct source identity and its
  indexed affinity/collation comparison implies one equivalence class under
  the grouping term's no-affinity comparator.

`IS NULL` fixes one NULL group. Ordinary `= NULL` produces empty input.
Non-deterministic source-independent expressions, including `random()`, are
never fixed.

Every remaining group term must match exactly one consecutive index key term.
The group-term order may be a permutation of the index prefix because
adjacency, not presentation order, is sufficient. Effective collations must
match. A range term may participate only when it is the first varying
grouping field.

`grouping_order` records the actual varying group-term order delivered by the
path. Its direction and NULL placement use the `effective_desc` rule above.
The vector contains no duplicate or out-of-range group-term index.

Among qualifying ordered paths, use the existing deterministic access
preference. A legal ordered path is preferred to external grouped sorting;
the immutable performance baseline in package 15 may tighten or replace that
preference through a reviewed contract version.

### 7. Fall back to stable external sorted grouping

When no input path proves grouping adjacency, use `kSortedGroup`.

The grouping sorter key contains every bound group term in bound order and
uses each term's collation with NULL-equal grouping semantics. The physical
node records the sorter delivery order:

- normally every term is ascending with default NULL-first placement;
- when the singleton statement ORDER BY is strictly identical and uses only
  default NULL placement, copy its directions and default NULL placement so
  the one group sorter supplies both grouping and final order.

The later grouping package owns sorter payload layout and execution. Package 9
publishes only the strategy and delivered-order proof.

### 8. Coalesce only a complete singleton aggregate order

A physical top-level order is marked satisfied only for a one-core statement
with no compound operator when:

- the aggregate node uses `kGlobal` or `kIndexedMinMax`; or
- every ORDER BY term is structurally equivalent to the corresponding GROUP
  BY term, has the same collation, and has default NULL placement; and
- `grouping_order` proves the complete requested directions in that exact
  term order.

Structural comparison follows result-alias references and compares immutable
bound payloads and finalized aggregate invocation identities. It performs no
new name, function, or collation lookup.

`at_most_one_row` is derived and validated, never assumed:

- `kGlobal` and `kIndexedMinMax` are always at most one row;
- grouped input is at most one row when its access is Empty, SingleRow, or an
  exact one-row lookup; or
- every grouping term has the fixed proof defined above.

Global and indexed-MIN/MAX output does not evaluate ordinary ORDER BY keys:
SQLite clears that order context for its no-GROUP-BY aggregate branch.
Grouped output retains the order context even when cardinality analysis proves
at most one group, because hidden volatile or failing ORDER BY expressions
still execute for an emitted group. A grouped plan marks order satisfied only
when exact GROUP BY/ORDER BY coalescing supplies the required key evaluation.
Grouping-only permutations do not satisfy final order, and nondefault NULL
placement never coalesces for grouped output. DISTINCT may preserve a global
aggregate's order satisfaction, but a compound never inherits per-arm
ordering as a global order.

### 9. Keep lowering and execution explicitly unsupported

Logical and physical planning now succeed for aggregate queries.

Lowering detects `PhysicalAggregateNode` before emitting any source,
temporary-storage, or result instruction and returns the existing typed
unsupported-feature error:

```text
aggregate lowering is not implemented
```

Read-session preparation therefore remains a prepare-time failure until
package 11, but the failure moves from planning to the correct next
architecture boundary. Package 10 may add aggregate bytecode and VM state
without making SQL execution public.

### 10. Verify plan shape, ownership, limits, and allocation

Development follows red-green-refactor. Tests cover:

- appended stable logical and physical kind names;
- global source, WHERE, aggregate, HAVING, LIMIT, and projection order;
- aggregate nodes in DISTINCT and compound arms;
- Empty and SingleRow inputs feeding global aggregation;
- rowid and ordinary-index MIN/MAX directions, equality-fixed arguments,
  collation, non-NULL-first/one-NULL-fallback metadata, noncovering
  requirements, and conservative fallbacks;
- ordered grouping from rowid and index prefixes, including group-term
  permutations, deterministic constants, compatible equality prefixes, and
  non-deterministic source-free terms;
- sorted grouping for expressions, collation mismatches, unsupported indexes,
  and missing ordered paths;
- exact and nonexact GROUP BY/ORDER BY coalescing, descending order, and
  nondefault NULL placement, including global at-most-one and grouped
  fixed-key cases;
- forward/reverse range start, end, advance, direction, and NULL-placement
  metadata;
- candidate stability for one-core plans;
- explicit lowering and public-session rejection;
- exact allocation budgets and exhaustive planner/optimizer OOM cuts; and
- allocation-free immutable plan access.

No aggregate state, step/final call, representative-row mutation, grouping
sorter, result row, DISTINCT aggregate relation, FILTER schedule, or local
aggregate ordering executes in this package.

## Consequences

### Positive

- Aggregate semantics become visible in both logical and physical plans
  before bytecode design.
- Existing indexes support the two required first optimizations without
  coupling the planner to VM state.
- Ordered grouping and exact final-order proofs are distinct, matching
  SQLite's `WHERE_GROUPBY` versus `WHERE_SORTBYGROUP` behavior.
- Global cardinality makes skipped ORDER BY work explicit.

### Negative

- Physical access analysis becomes reusable per core rather than remaining a
  single-statement shortcut.
- Reverse traversal is published before aggregate lowering consumes it.
- Structural order comparison adds planner logic over immutable bound
  expressions.

## Rejected alternatives

### Add aggregate strategy only during lowering

Rejected because index choice, grouping order, and sorter necessity are
physical-plan decisions that must remain inspectable before bytecode.

### Treat every index grouping path as final ORDER BY

Rejected because an index may make equal group keys contiguous in a different
term order without delivering the requested global order.

### Always sort GROUP BY

Rejected because existing-index grouping and its LIMIT/error schedule are
required in the first SQLite-aligned aggregate implementation.

### Always scan all rows for MIN/MAX

Rejected because existing-index early-out is a required first-version
mechanism and affects observable evaluation skipping.

### Make aggregate planning execute through ordinary projection

Rejected because aggregate calls and representative leaves are not eager
source-row scalar expressions.

## References

- ADR-0003: Dependency Direction and Layering
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0034: Immutable Logical SELECT Plans
- ADR-0035: Deterministic Basic Read Optimization
- ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0060: SQLite-Aligned Aggregate Binding
