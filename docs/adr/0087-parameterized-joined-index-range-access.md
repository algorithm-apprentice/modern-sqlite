# ADR-0087: Parameterized Joined Index Range Access

- Status: Accepted
- Date: 2026-10-15

## Context

PR #197 completes ADR-0086's noncovering joined equality prerequisite.
Node `implement-advanced-sql` remains active. The canonical index analyzer
already describes an optional equality prefix followed by one range, but
joined admission and register layout explicitly exclude that range.

The existing implementation provides:

- shared constraint orientation, value-key readiness, affinity/collation
  admission, STAT1 costs, and physical validation;
- logical inclusive/exclusive lower and upper bounds on the next index term;
- ordinary covering/noncovering and global aggregate range producers;
- indexed MIN/MAX's borrowed traversal metadata and direction handling;
- ordered key-expression evaluation and one explicit-destination key-block
  emitter with canonical affinity application;
- source-owned retained equality blocks, paired noncovering cursors, and one
  immediate query rowid scratch; and
- exact exhaustion, virtual NULL rows, and complete matched-ledger replay
  through one shared accepted suffix.

Twenty-eight read-only, profile-validated SQLite 3.54.0 probes reuse both
frozen indexed fixtures. Bounded and equality-prefix ranges use stored-index
searches. Native may instead scan a tiny ordered input; this slice does not
replace the canonical cost model to force identical plans. Covering outer
replay can traverse a complete index, whereas noncovering replay scans the
complete table. A private scalar using a selected descending numeric range
returns `same-one`, not the first qualifying row in table order. Unsafe
numeric-affinity comparisons against TEXT indexes remain scans.

The unchanged 4,096-row fixture has a native two-sided range search for both
covering and table-backed consumers, returning count 1. Modern currently
rejects the range candidate and scans the inner table. Record its actual
fixed work before changing production.

## Decision

### 1. Admit the existing constrained range shape

Extend the existing joined candidate to accept an optional leftmost
`=`/`IS` prefix followed by one range term with `>`, `>=`, `<`, or `<=`.
At least one equality or range must constrain the index. A range need not
have an equality prefix; unconstrained joined index scans remain excluded.

Reuse the canonical analyzer, bound orientation, actual-core field discovery,
cost publication, and stable candidate preference. Keep value-only key
classification, earlier-source readiness, comparison affinity, effective
collation, own-ON preference, WHERE preservation barriers, and rowid
point-lookup priority unchanged.

Retain physical forward traversal for joined sources, including DESC terms.
Do not add range-specific estimates or alter STAT1, `unordered`, unselective
fallback, supported index shapes, or ordinary reverse traversal. No syntax,
physical node kind, opcode, or VM capability is added.

Computed/function keys, skip-scan, automatic transient indexes, reordered
joins, partial/expression indexes, and WITHOUT ROWID secondary-index access
remain separate requirements, not new unsupported-SQL errors.

### 2. Promote the existing traversal mechanism instead of copying it

Move indexed MIN/MAX's borrowed start/end traversal metadata and its common
normalization into the existing internal lowering helper boundary. Resolve
logical bounds into physical start/end roles from the range term's ASC/DESC
order and the scan direction.

Ordinary covering/noncovering, global aggregate, indexed MIN/MAX, and joined
callers share this resolver and the typed seek/end-check mode emission.
Keep MIN/MAX's special NULL passes in its existing policy; a common range
helper must not replace those distinct passes.

Helpers borrow immutable physical metadata and append existing typed
instructions. They own no cursor, register, key container, or runtime state.
Callers retain expression evaluation, open phases, labels, first/subsequent
candidate acquisition, and typed error mapping. Preserve ordinary emitted
order and first-candidate table-open behavior rather than refactoring an
entire producer into a new executor or service.

### 3. Retain the end key in the existing source block

For `E` equality terms, allocate the source-owned contiguous block with
`E + 1` registers when a range exists, otherwise retain `E`. Lower and upper
expressions use the existing expression homes; they do not acquire a second
per-source value array.

Evaluate the selected equality and range expressions once per logical source
entry in original conjunct order. Reuse the explicit-destination block
emitter to copy/apply affinity in physical term order.

Copy the physical start key before the seek, or rewind when there is no
physical start key. If a physical end bound exists, immediately after
successful positioning copy the physical end key into the same source block
before binding the row-loop backedge and before any suffix or child query can
overwrite expression homes. The start range value is no longer retained
after the seek. An equality-only end check reuses its already retained prefix
without another copy. Every candidate checks the retained end block without
reevaluating its key expressions.

An absent physical range bound may still leave an equality-prefix check.
If neither an end bound nor an equality prefix exists, emit no end check.
Initialize the entire retained capacity to NULL before any preserving or
replay path can bypass normal positioning, including the range slot.
Do not use the pooled ordinary workspace as retained joined state.

### 4. Preserve the accepted source and replay state machine

Runtime NULL range keys take the existing unpositioned no-match edge. A
successful range position follows the same own-ON, match-ledger, WHERE, and
consumer phases as equality access; the complete original predicates remain
at their existing readiness and preservation boundaries.

Check the index end before any noncovering table lookup. Reuse ADR-0086's
canonical suffix validation, exact table seek, companion invalidation before
normal advance, and paired virtual NULL publication. Normalize a positioned
out-of-range index through typed NULL-row disable before ordinary exhaustion.
EOF and NULL rejection retain the exact existing unpositioned proof.

RIGHT/FULL replay ignores every normal range bound and scans the complete
existing replay driver: the index for covering access, the table for
noncovering access. Reuse the same identities, ledger, synthetic prefix,
parked-index normalization, and accepted suffix. Do not reevaluate own ON,
copy the suffix, or relax cursor/register verification.

Preserve source acquisition, LIMIT 0 bypass, early LIMIT, suspension,
reset/rebind, failure, finalize, and destruction. All existing consumers,
including private scalar/EXISTS regions, use this one producer.

## Verification

Review this design before production changes. Begin from merged prerequisite
`73640abf2572bf25b0b8afebedf13aa3f165f999`, whose complete local and required
hosted gates are green.

Observe a missing joined range leaf and a genuine public fixed-work red
before implementation. Use the unchanged indexed fixture and:

```sql
SELECT count(b.payload)
FROM items a JOIN items b
  ON b.category>=a.category AND b.category<=a.category
WHERE a.id=1
```

Exclude prepare, retain result count 1, record actual before/after
instructions, and require fewer than 256 instructions after integration.
Retain the equality controls at 35 covering and 41 noncovering instructions,
and the rowid control at 364. Do not change any frozen benchmark input,
baseline, budget, threshold, profile, or timeout, or claim wall-time speedup.

Pin one-/two-sided and prefix-free/prefixed ranges, inclusive/exclusive and
reversed operands, ASC/DESC, NULL runtime/stored keys, contradictions, mixed
affinity/collation, IS prefixes, readiness/preservation barriers, and actual
compound-core ownership. Prove nested end-key retention and one shared rowid
scratch across joined and ordinary/global consumers.

Cover all join kinds and consumers, empty inputs, complete out-of-range
replay, matched-but-WHERE-rejected rows, order-sensitive matched native
access, typed reset/rebind, zero/early LIMIT, both-root and suffix corruption
failures, and bounded 64-source compilation/execution.

Extend existing module OOM, independent-model, external matched-ledger
failure/recovery, and grammar-aware sanitizer evidence through their
canonical helpers. Preserve all inherited corpus cases and oracle records
as JSON data. Only declared coverage and the computed new corpus digest
change alongside appended native cases; frozen oracle provenance and
fixture bytes remain unchanged.

Run the focused tests, complete fast tier, required compiler/sanitizer/
quality gates, and unchanged archived performance contracts. Obtain
GPT-5.6 Sol design and implementation/closure review before committing
production. Merge only the reviewed exact head after every required hosted
gate passes, clean only that merged branch, and continue automatic-index
integration and the remaining prerequisite-ready ADR-0053 slices.
This package does not complete node 43.

### Local closure evidence

Four focused tests fail against the unchanged merged production base before
implementation. Both public range workloads return count 1 while executing
36,888 instructions. After integration, covering range execution uses 44
instructions and noncovering range execution uses 49, below the unchanged
exclusive bound of 256. Equality controls remain 35 covering, 41 noncovering,
and 364 rowid instructions. These are execution-only fixed-work observations,
not wall-time speedup claims.

The expanded focused suite passes 44 tests. The independent preservation
model retains its original equality/IS coverage and now checks 432
combinations. Module evidence includes nested retained end keys and backedge
placement, shared rowid scratch across joined/ordinary/MIN/MAX consumers,
complete covering and table-backed replay, real empty inputs, suffix
corruption, selected-root read failures, typed reset/rebind, external
matched-ledger failure/recovery, and bounded 64-source execution. Existing
OOM sweeps retain their allocation positions, caps, and timeouts.

The pinned corpus adds 62 comparisons for 470 comparisons and six unchanged
unsupported boundaries. All 414 inherited corpus cases and oracle records,
frozen provenance, and fixture bytes remain unchanged. The complete
Modern/native comparison passes.

Complete Debug, Release, and ASan/UBSan tiers each pass 1,361 tests; genuine
GCC 16 Debug and Release each pass 1,359. The complete instrumented fast unit
tier passes 1,326 tests. All 58 archived performance contracts pass without
changing frozen inputs, baselines, budgets, or thresholds. The final
ASan/UBSan grammar campaign completes 20,000 inputs with seed 87, maximum
input length 4,096, and the unchanged ten-second per-input limit. Required
unfiltered production clang-tidy, pinned formatting, engineering discipline,
and dependency-graph validation pass.

Final review and exact-head hosted gates remain mandatory before merge.
This evidence does not complete `implement-advanced-sql`.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 joined access/scaling prerequisites.

Contracts: ADR-0033, ADR-0035, ADR-0036, ADR-0052, ADR-0053, ADR-0067,
ADR-0072, ADR-0076, ADR-0077, ADR-0079, ADR-0080, ADR-0081, ADR-0084,
ADR-0085, and ADR-0086.
