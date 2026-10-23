# ADR-0146: Native Joined Rowid Range Access

- Status: Accepted
- Date: 2026-10-24
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12 and its planner prerequisites

## Context and source map

PR #248 delivers ADR-0145's ordinary joined/prefix producers. Its retained
ID-only projection control still chooses a covering outer scan where SQLite
uses a bounded rowid traversal. The engines can select different outer
representatives for an unspecified tied ordering key. This is not a claim
that SQL defines the tied representative; it identifies an existing native
access mechanism that Modern explicitly excluded.

The merged baseline is `562bb48fdab91baf7872979c0ffc85f2dc771cf1`.
Its accepted tree and matched capture provide the before baseline. Thirteen
local, profile-validated controls retain the original plans, rows and error
phases without changing a committed corpus or oracle.

| Responsibility | Pinned SQLite / existing Modern owner |
|---|---|
| Compatible outer constraints and key prerequisites | `where.c:832-853,3300-3327`; `ScanConstraints`, normalized term masks |
| Lower/upper alternatives and native range costs | `where.c:3428-3477,3555-3606`; `AddRowIdRange`, `RangeEstimate`, existing N-best solver |
| Delayed end evaluation and directional traversal | `wherecode.c:1768-1875`; `RowIdTraversal`, typed rowid seek and table cursor |
| Exact occurrence consumption | `wherecode.c:419-438`; `AssignJoinedTermPhases` |
| Numeric seek conversion and failure | `vdbe.c:4967-5065`; existing `ConvertRowIdSeek` and `SeekRowIdInstruction` |
| Complete unmatched replay | `wherecode.c:2893-2995`; existing joined producer and RIGHT matched relation |
| Registers, predicates, output and cleanup | `JoinedSourcePlan`, query register layout and `EmitJoinedLoop` |

The planner already constructs rowid range candidates for one source.
Physical metadata, validation, ordering proof, traversal resolution, seek
conversion and ordinary/aggregate consumers already support that shape.
Joined planning alone excludes it, and the joined producer currently has no
range positioning/end-check branch.

## Decision

### Admit the existing candidate and physical leaf

Remove only the single-source restriction on rowid range enumeration.
Publish the selected existing `PhysicalRowIdRangeNode`, including the
solver's direction, through `BuildJoinedAccessLeaf`.

Reuse the same normalized constraints, source prerequisites, outer barriers,
native estimates, candidate pruning and path selection. Preserve original
predicate occurrence/origin identities and canonical term consumption.
Do not introduce a second range analyzer, cost policy or access choice in
lowering. Existing unsupported source and access kinds remain unsupported.

### Extend the canonical joined producer

For each normal logical source entry:

1. Resolve start/end roles through the existing `RowIdTraversal`.
2. Evaluate the start once and emit the existing typed seek, or Rewind/Last
   when no start exists.
3. Only after positioning succeeds, evaluate the end once into this source's
   retained key register.
4. Before ON, match publication, WHERE or any child source, read the current
   rowid and perform the native numeric end comparison; true or NULL stops.
5. Reuse the existing row body, Next/Previous and completion continuations.

Use the existing source-owned key block, generalizing its private
`index_key_*` names to `access_key_*`. A rowid range needs one retained slot
only when its resolved end exists. Do not retain the end in a reusable
expression home or add another key container.

Reuse the existing query-owned rowid-range rowid/comparison scratch. Extend
its need calculation to joined sources; nested heads use it sequentially,
while each source's end value remains independently owned. Extract the
existing numeric end-check emission into one internal helper used by the
ordinary, streaming, global-aggregate and joined callers. This helper owns
no registers, cursor, plan or runtime state.

Normalize a positioned range-end exit through existing `NullRow(false)`
before joining seek-miss/EOF cleanup. Preserve the verifier's cursor and
initialized-register rules. Initialize retained slots through the existing
preserving/replay initialization path.

LEFT/FULL virtual publication skips the normal bound checks. RIGHT/FULL
unmatched replay scans the complete table and reaches the existing accepted
suffix without reevaluating own ON or normal bounds. Existing replay-mode
advance, matched identity and WHERE phases remain authoritative.

Keep ordered-inner rejection, prefix block production, scalar demand,
LIMIT/OFFSET, reset/rebind, errors and cancellation in their existing
producer. No VM instruction, executor, sorter, cursor owner or query frame
is added.

## Evidence and delivery

Deliver one coherent implementation commit after design review. Start with
focused failures for the missing physical access, the retained native
ordering control and execution-only bounded-range work. Then qualify:

- parameterized one-/two-sided bounds, reversed operands and selected
  forward/reverse traversal;
- empty/missing/NULL starts, delayed erroring ends, inclusive/exclusive
  numeric boundaries and existing TEXT/REAL/BLOB seek conversion;
- nested retained ends, original term consumption and source reordering;
- LEFT/RIGHT/FULL virtual rows and complete replay, scalar/EXISTS,
  aggregate/compound consumers, early stop and reset/rebind;
- malformed physical metadata and existing optimizer/lowering/session OOM
  paths, with unchanged allocation positions, caps and deadlines.

Reuse owning-module fixtures and helpers. Keep the complete fast tier,
selected isolated compiler/sanitizer/model checks, actual changed-TU quality,
matched fixed work/native contracts and required exact-head hosted gates.
Review design and implementation with GPT-6.1 Sol before their commits.
Preserve every existing workload, oracle, assertion and threshold.

Actual compound scheduling, non-natural NULL ordering, reverse automatic
access and final VM classification remain separate work. This access
prerequisite neither completes node 43 nor starts nodes 44-48.
