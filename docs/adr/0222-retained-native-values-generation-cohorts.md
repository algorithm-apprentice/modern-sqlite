# ADR-0222: Retained Native VALUES Generation Cohorts

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #300 main `4fb8bd45`, ledger257
- Related contracts: ADR-0106/0111, ADR-0159/0192 and ADR-0218/0219/0221

## Completed partition mapping and current requirement

The remaining generation owner needs actual native creation boundaries,
not only expression equality or an eager/filtered schedule. Native eager
constructor code is shared through real alias copies. The existing physical
scratch and invocation-once contracts do not authorize a global result cache
or shared mutable coroutine registers.

One smaller prerequisite is already present internally but lost at publication.
For:

```sql
VALUES(7),(8),(9 IS 9),(10);
```

Pinned SQLite creates two eager coroutine definitions: rows0-1 and rows2-3.
The incoming third row stops the no-IS constant walk. The fourth row then
allows the third row to start another producer through the normal first-row
walk. Both completed cohorts are eager, but they are not one creation site.

Retained source241 native EXPLAIN and Modern binding observations distinguish
this case from one eager run and a mixed eager/fallback case. They are
historical research, not a new main257 execution or a behavioral red.
Current direct source inspection confirms the publication condition still
discards the two-cohort partition.

| Responsibility | Pinned or current source |
| --- | --- |
| First-row normal constant/no-affinity check | `sqlite3.c:139836-139847` |
| Incoming no-IS gate, fallback and coroutine creation | `sqlite3.c:139901-139979` |
| Incoming row generation and producer completion | `sqlite3.c:139822-139829,139983-140003` |
| Canonical native constructor partition | `NativeValuesConstruction`, `ConstructValuesRow` |
| Current uniform-schedule compaction | `BindValuesQueryCore`, `bound_select.cpp:4027-4036` |
| Existing persistent partition carrier | `BoundValuesCore::chunks`, `BoundValuesChunk` |
| Canonical prepared expression roots | `SyntaxNormalization::construction_binding`, `BindExpression` |
| Existing per-occurrence source-type owner | ADR-0192; `RecordValuesTypeInfoSource` |
| Current row-schedule consumer | `EmitStreamingValuesCore`, `plan_lowering.cpp:13878-13955` |
| Owning metadata and failure helpers | Binder chunk/capture tests and `bound_select_oom_test.cpp` |

`ConstructValuesRow` already computes the complete ordered partition with
the native first/incoming distinction. Source-type preparation already uses
that uncompressed private owner. `BindValuesQueryCore` copies it into the
existing public bound owner only when schedules differ, erasing distinct
eager cohorts when all final schedules match.

The current row producer uses those chunks only to select each row's existing
schedule and validate coverage. Retaining equal-schedule boundaries therefore
supplies required metadata without itself changing callback counts, sharing
coroutines or completing the general generator.

## Decision

### Preserve the existing native partition when eager boundaries matter

Keep the current private construction algorithm and the existing
`BoundValuesChunk` representation. At bound VALUES publication, retain the
complete construction partition when it contains multiple chunks and at
least one producer-eager chunk.

A single eager producer keeps the compact existing schedule. An entirely
consumer-filtered core also remains compact: its ordinary rows remain
explicit in the existing row array and have no early eager producer to share.
Mixed eager/fallback cores retain exactly their current partition.
Uniform multiple-eager cores now retain their already-computed boundaries.

Use the existing publication scan and vector copy; do not reconstruct ranges,
merge adjacent eager cohorts, add another container or traverse syntax again.
Copy the original row beginnings, counts and schedules without sorting or
changing them. The retained partition still covers every row exactly once,
with nonempty contiguous chunks, and has at most one chunk per source row.

Keep `BoundValuesCore::schedule` unchanged. An empty chunk vector still means
that existing consumers can use that schedule for every row. It no longer
means that every uniform schedule must be compacted. For an eager core, the
compact form now preserves the fact that there is only one native cohort.
This is not a new global generation-site identifier.

### Preserve expression, preparation and row ownership

Keep every row and canonical bound expression ID. Actual query/template
copies may retain their own bound metadata while borrowing the same prepared
constructor roots. Do not rebind, clone or replay successful roots, and do
not add a second expression cache.

Do not replace ADR-0192's private construction/work-arena owner with the
published chunks. That arena records source occurrences and diagnostic
ordering, not merely a runtime row partition. Preserve first-row type work,
alias replay, names, collation, width, parameter and construction-error order.

Leave parser, planner, lowering and VM behavior unchanged. The current row
emitter consumes the same schedule for every row, including EXISTS, OFFSET
and LIMIT boundaries. Do not allocate a new coroutine, once site, register or
cached value merely because the additional metadata is retained.

The existing vector owns the additional retained records. Let allocation
failure unwind through the current binder/compiler and session boundaries;
do not add a catch, partial publication or success-shaped fallback. Preserve
all existing syntax, row, depth, allocation and process limits.

### Narrow supersession

Supersede only ADR-0159 section4's requirement that every uniform core retain
the compact schedule. Distinct eager producer boundaries now survive public
bound publication. Its native construction, canonical binding, row evaluation
and ownership contracts remain unchanged.

ADR-0192 continues to use its original complete construction partition.
ADR-0218/0219/0221's private-register, once and initialization restrictions
remain intact. This source does not authorize shared coroutine callers,
ordinary constant-function activation, deferred initialization or diagnostic
arbitration, signed-alias generation, a fake query wrapper or IN.

## Focused proof and bounded delivery

Add one owning binder regression for the missing two-cohort metadata before
changing production. Record its actual failure, then make the smallest
publication correction. Cover another restart boundary, existing single and
mixed controls, compact ordinary rows, and retained canonical roots through
real alias/query copies.

Extend the existing complete allocation-prefix sweep with a multi-cohort
constructor/alias case. Keep every existing case, ceiling and failure
expectation; do not substitute the separate sampled-allocation selection.
Preserve binding recovery and owner lifetime after injected failures.

Verify that each row keeps its original schedule and that existing lowered
rows, preparation winners and non-target constructor/alias controls remain.
Record actual retained-vector allocation and byte changes; public carrier
layout and generated program resources must not grow. This is a metadata
prerequisite, not a speed improvement or a new timing family.

Run focused green, complete fast units and the actual change-aware closing
plan, with outstanding genuine compiler and hosted domains explicit.
Independent design acceptance precedes implementation; independent code and
source review precede a normal coherent commit and append-only ledger entry.
A dependent generation slice may follow only after that source is accepted.
Final exact-head/integrated/main qualification and expected-OID cleanup remain
required. General generation, IN, node43 and the project remain incomplete.
