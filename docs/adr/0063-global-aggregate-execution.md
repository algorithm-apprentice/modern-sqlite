# ADR-0063: SQLite-Aligned Global Aggregate Execution

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0062 provides verified aggregate bytecode and VM state, while ADR-0061
still blocks every aggregate physical plan at lowering. Package 11 must make
the first aggregate SQL subset publicly executable without pulling grouped
execution, aggregate modifiers, or the final integration matrix into one
change.

The merged contracts already provide:

- bound global aggregate invocations in result, ORDER BY, and HAVING order;
- global and indexed-MIN/MAX physical strategies;
- source and representative leaf analysis;
- typed aggregate open/step/finalize/reset/close bytecode;
- table, rowid, covering-index, and noncovering-index access lowering;
- strict LIMIT/OFFSET initialization and lazy projection; and
- read-session prepare/step/reset/finalize ownership.

Pinned SQLite 3.54.0 defines the relevant schedules in:

- `src/select.c:8889-9030`, for global accumulator reset, source stepping,
  MIN/MAX early-out, finalization, HAVING, and output;
- `src/select.c:2573-2618`, for LIMIT-before-OFFSET initialization and LIMIT
  zero source bypass;
- `src/where.c:114-137`, for stopping an ordered MIN/MAX loop after the first
  qualifying step; and
- `src/where.c:6200-6260`, for forward/reverse ordered access and nullable
  MIN positioning.

Pinned behavior additionally establishes:

- empty global input still finalizes every invocation and may emit one row;
- LIMIT zero performs no source or aggregate work;
- OFFSET may discard the one candidate row without evaluating projection;
- top-level ORDER BY expressions are not evaluated merely to sort global
  output, but aggregate invocations discovered there still step and finalize;
- every invocation steps and finalizes in bound invocation order;
- HAVING executes after all finalizers and before result projection;
- a matching MIN/MAX path evaluates only rows needed to find the first
  qualifying non-NULL extremum, with one qualifying NULL fallback when no
  non-NULL row exists; and
- statement reset reruns aggregate initialization from a clean state.

## Decision

### 1. Execute a bounded first global subset

Package 11 accepts one ordinary, non-DISTINCT SELECT core with:

- a `BoundAggregateProgram` whose GROUP BY list is empty;
- physical strategy `kGlobal` or `kIndexedMinMax`;
- zero or more ordinary aggregate invocations;
- optional WHERE, HAVING, LIMIT, OFFSET, and top-level ORDER BY already marked
  `input_order_satisfied=true`; and
- result and HAVING expressions composed from aggregate calls, constants,
  parameters, scalar/lazy expressions, and operators.

This package explicitly rejects:

- DISTINCT SELECT;
- compounds;
- grouped aggregate strategies;
- aggregate DISTINCT, FILTER, or local ordering; and
- representative source requirements reachable from result or HAVING
  expressions, including bare source-column result or HAVING expressions.

Representative snapshots are owned by package 12 and modifier schedules by
package 13. Unsupported shapes retain a typed lowering error rather than a
success-shaped fallback.

Source leaves reachable only from the satisfied global ORDER BY are ignored:
ordinary ORDER keys are not evaluated. Aggregate invocations discovered in
those expressions remain in the invocation table and still execute.
Aggregate access covering analysis is amended to exclude such ORDER-only
representative leaves.

### 2. Add reverse cursor bytecode required by MIN/MAX

Append:

```cpp
struct LastInstruction {
  CursorId cursor;
  InstructionAddress empty_target;
};

struct PreviousInstruction {
  CursorId cursor;
  InstructionAddress previous_target;
};
```

They mirror Rewind/Next verifier states and VM behavior but use
`TableBtreeCursor::Last/Previous` or `IndexBtreeCursor::Last/Previous`.

Extend `IndexRangeEndMode` with reverse inclusive/exclusive modes. Range roles
are derived in two stages:

1. map logical lower/upper bounds into physical index order using
   `PhysicalIndexRange::order`; and
2. choose the physical start and end from that pair using
   `PhysicalScanDirection`.

Forward inclusive/exclusive checks stop on `current > end` /
`current >= end`. Reverse inclusive/exclusive checks stop on
`current < end` / `current <= end`. Bound expressions retain original WHERE
evaluation order; only their physical seek/end roles are swapped.

Existing enum values and instruction variant indices remain unchanged by
appending new values and alternatives.

ProgramBuilder gains label-resolving `EmitLast` and `EmitPrevious` helpers.

### 3. Inspect one exact global physical chain

Lowering accepts:

```text
Access
  -> optional WHERE Filter
  -> Aggregate
  -> optional HAVING
  -> optional satisfied Order
  -> optional Limit
  -> Projection or Output
```

The access leaf may be Empty, SingleRow, table scan, rowid lookup, covering
index, or noncovering index. The aggregate node must be core zero and
`at_most_one_row=true`. A physical order node is accepted only when
`input_order_satisfied=true`; it allocates no sorter/top-N descriptor and
ordinary order keys are not emitted.

The lowering guard remains for grouped, DISTINCT, compound, representative,
or modifier-bearing aggregate plans.

The implementation separates plan inspection, register/descriptor layout,
source positioning, range checks, candidate stepping, cursor-state
normalization, and nullable fallback into focused helpers. Access emitters
compose those operations; they do not duplicate one monolithic global
aggregate algorithm.

ADR-0067 isolates the access-shape dispatch, cursor control flow, and indexed
MIN/MAX helpers in an internal lowering module without changing this
execution contract.

### 4. Allocate one descriptor, state slot, argument block, and result register per invocation

For every invocation in `BoundAggregateProgram::invocations`, lowering:

- interns the bound aggregate function name and invocation collation;
- adds one `AggregateDescriptor`;
- allocates a contiguous argument block of descriptor arity;
- allocates one finalized-result register; and
- records the mapping from statement-global
  `BoundAggregateInvocationId` to descriptor and result register.

Deduplicated aggregate-call expressions copy from the same final register.
Invocation order remains the bound program order.

No representative-effect output register is allocated in package 11 because
representative leaves are rejected.

### 5. Initialize LIMIT and guards before aggregate state

Lowering emits:

1. LIMIT evaluation and strict integer conversion;
2. LIMIT-zero completion jump;
3. OFFSET evaluation and conversion;
4. aggregate Open instructions;
5. source-independent guards; and only then
6. source initialization.

The pre-open completion path halts directly. It never opens, finalizes, or
closes aggregate state.

Guard rejection represents empty input and therefore joins an opened
empty-input completion that finalizes every invocation. It never halts
directly.

Source paths do not merge incompatible cursor states directly. Each access
shape emits state-specific closing shims for closed, unpositioned, and
positioned cursors, including separate index/table combinations for
noncovering access. Every shim closes the live cursor set and then joins the
common aggregate-finalization label with all cursors Closed and aggregate
slots Accumulating. Duplicating the short finalization schedule is permitted
when a shared normalized label would increase verifier complexity.

### 6. Step ordinary global invocations in invocation order

For every accepted source row:

1. evaluate one invocation's ordinary arguments left to right into its
   contiguous argument block;
2. emit Step without representative-effect output; and
3. continue with the next invocation.

Only after every invocation steps does the source cursor advance.

Package 11 rejects invocation modifiers before descriptor allocation. Source
expression, conversion, aggregate-step, cursor, and I/O failures propagate
immediately through existing VM cleanup.

### 7. Finalize, close, apply HAVING/OFFSET, then project lazily

At opened completion:

1. finalize every invocation in invocation order into its result register;
2. close every finalized aggregate slot;
3. evaluate HAVING and halt on false or NULL;
4. apply OFFSET to the single candidate row;
5. evaluate result expressions lazily; and
6. emit at most one ResultRow followed by Halt.

`EmitExpression` handles `BoundAggregateCallExpression` by copying the mapped
final register. Aggregate arguments are never re-evaluated during HAVING or
projection.

Projection errors occur only after OFFSET accepts the candidate. HAVING errors
occur after every aggregate finalizer, matching SQLite.

### 8. Execute Empty and SingleRow inputs without special aggregate semantics

Empty physical input opens and finalizes every invocation without Step.

SingleRow input applies WHERE once and either:

- steps every invocation once; or
- reaches empty finalization when WHERE rejects the synthetic row.

The same opened-completion block serves table, rowid, and index access.

### 9. Implement indexed MIN/MAX early-out exactly

`kIndexedMinMax` has one plain MIN/MAX invocation and no HAVING or modifiers.
Lowering validates the physical `PhysicalMinMaxPlan` against the bound
invocation before emission.

For a non-NULL argument or rowid:

- position the selected table/index cursor at the requested extremum;
- apply selected range/equality access and residual predicates;
- step the first qualifying row; and
- jump to the appropriate positioned-cursor closing shim, which closes the
  live cursor set before joining opened completion.

For `kNonNullFirstWithSingleNullFallback`:

1. position at the first possible non-NULL extremum, skipping the NULL region
   without evaluating residual predicates;
2. scan in extremum order until one residual-qualifying non-NULL row steps,
   then complete;
3. if no qualifying non-NULL row exists, position in the NULL region;
4. evaluate residual predicates in the selected scan direction; and
5. step exactly one qualifying NULL row, then complete.

NULL fallback eligibility is derived from the selected constraint on the
aggregate argument:

- no argument constraint permits fallback;
- `IS` with a runtime NULL key enters the NULL pass directly;
- non-NULL `IS` or `=` excludes fallback;
- every lower or upper range on the argument excludes fallback;
- ordinary `= NULL` and a NULL range key produce empty input; and
- equality constraints on unrelated prefix fields do not alter argument
  fallback.

An equality-fixed non-NULL argument may stop after the first qualifying row.
Selected predicates remain enforced by access positioning. The fallback never
visits rows excluded by a selected equality or range, and only residual
predicates execute inside candidate loops.

Covering and noncovering paths retain existing table lookup and residual
ordering. Reverse range lowering derives start seek, end check, and cursor
advance from `PhysicalScanDirection` while preserving the logical bound
expressions.

Package 11 still rejects representative leaves, so MIN/MAX step effects are
not requested. The NULL step remains required for exact state and future
schedule compatibility.

### 10. Preserve public statement reset and metadata

Read-session preparation now succeeds for the accepted global subset.

Statement reset:

- invokes VM reset, which destroys every aggregate state;
- retains parameter bindings under the existing session contract;
- reruns Open, Step, Finalize, and Close on the next execution; and
- reproduces empty and nonempty results without retained accumulator state.

Aggregate result columns retain binder-published names, no declared type, and
NONE affinity. No session-specific aggregate metadata path is introduced.

### 11. Verify direct schedules and public behavior

Development follows red-green-refactor. Tests cover:

- global COUNT/SUM/TOTAL/AVG/MIN/MAX/GROUP_CONCAT/STRING_AGG results;
- empty tables, all-NULL inputs, no-FROM input, WHERE-empty input, and
  multiple invocations;
- argument and invocation order, immediate step errors, delayed final errors,
  and finalization order;
- HAVING true/false/NULL/error and aggregate references;
- LIMIT zero bypass, invalid LIMIT before OFFSET/source, OFFSET projection
  skipping, and negative LIMIT/OFFSET;
- ignored top-level ORDER BY scalar keys plus aggregate invocations found
  there;
- forward/reverse rowid and index MIN/MAX, equality/range bounds, nullable
  non-NULL-first and one-NULL fallback, covering/noncovering paths, and
  residual call schedules;
- explicit rejection of grouped, DISTINCT, compound, representative, and
  modifier-bearing plans;
- prepare/step/reset/finalize metadata and parameter retention;
- exact allocation budgets and exhaustive lowering/session OOM cuts; and
- existing model, fuzz, sanitizer, compiler, and compatibility regressions.

No GROUP BY execution, representative snapshots, aggregate DISTINCT, FILTER,
local ordering, or aggregate temporary relation opens in this package.

Delivery update: ADR-0068 supersedes the grouped-execution and representative
snapshot deferrals while retaining this ADR's global schedules unchanged.
ADR-0069 subsequently supersedes the aggregate-modifier deferral for the same
accepted single-core global statement boundary.

## Consequences

### Positive

- The first useful aggregate SQL subset becomes public without weakening
  grouped or modifier contracts.
- Global evaluation order, LIMIT/OFFSET skipping, and ORDER elimination are
  explicit in bytecode.
- Indexed MIN/MAX uses the physical proof rather than rediscovering planner
  decisions in the VM.
- Reset reuses immutable programs while recreating every accumulator.

### Negative

- Reverse cursor instructions and reverse range checks enter bytecode before
  general ORDER BY elimination uses them.
- Indexed nullable MIN/MAX requires a two-phase source loop.
- Bare representative columns remain explicitly unsupported for one package.

## Rejected alternatives

### Lower global aggregates through ordinary projection

Rejected because aggregate calls are finalized state references, not eager
source-row scalar functions.

### Evaluate top-level ORDER BY keys for one global row

Rejected because SQLite removes that work and hidden errors/callbacks are
observable.

### Finalize only invocations referenced by the result list

Rejected because ORDER BY and HAVING invocations still step and finalize even
when their ordinary expression is otherwise skipped.

### Emit one state slot per aggregate-call occurrence

Rejected because binding already deduplicates equivalent invocations and
publishes their exact lifecycle identity.

### Fall back from indexed MIN/MAX to a full aggregate scan in lowering

Rejected because planner-selected early-out changes observable evaluation and
error schedules.

## References

- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0036: Physical Read Plan Lowering
- ADR-0037: RAII Read Session API
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0059: SQLite-Aligned Aggregate Function State
- ADR-0060: SQLite-Aligned Aggregate Binding
- ADR-0061: SQLite-Aligned Aggregate Planning
- ADR-0062: Typed Aggregate Bytecode and VM State
