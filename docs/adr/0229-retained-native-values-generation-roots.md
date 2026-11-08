# ADR-0229: Retained Native VALUES Generation Roots

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #305 main `10579bcd`, ledger281
- Related contracts: ADR-0106/0111, ADR-0159/0192 and ADR-0222/0225/0226

ADR-0233 enriches the inventory element and borrowed accessor from expression
IDs to original generation-site records. Its expression member retains the
canonical ID; this ADR's ordering, cache-publication and lifetime rules remain.

## Confirmed ownership gap

Native multirow VALUES generation happens during construction, before an
outer expression can discard the query. A later consumer's reachability is
therefore not the complete inventory of native value-generation requests.

On the qualified main, fresh paired native, ReadSession and WriteSession
controls include:

```sql
SELECT 1 OR (
  VALUES ((abs(-9223372036854775808) COLLATE BINARY + 0) + 0), (1)
);
```

Both engines prepare successfully. Native fails the first step with
`integer overflow`; both Modern facades instead return1. The incoming-row,
empty-consumer and pruned-IN-consumer variants retain the same distinction.
Direct unused function roots correctly remain unexecuted in both engines.
A reached-expression control fails in both, while a prior missing name
still prevents successful preparation.

These eight controls are research, not a behavioral red or a completed
general-generation design. The existing81-generation/11-signed-alias
observations were separately reconciled against identical source274 binaries
and owning source trees; they were not relabelled as fresh execution.

The native construction-function flag, temporary/requested destination
rules and deferred queue explain why retaining only runtime-used queries
cannot support the remaining owner. This decision preserves the missing
request inventory; it does not yet execute those requests or repair the
runtime differences above.

## Completed canonical inventory

| Responsibility | Pinned or current source |
| --- | --- |
| Start the native first-row producer and generate incoming rows | `sqlite3.c:139901-140003` |
| Finish an existing producer without regenerating its rows | `sqlite3.c:139822-139829` |
| Distinguish reached function-bearing and deferred generation | `sqlite3.c:118698-118740` |
| Skip deferred drain after an earlier error | `sqlite3.c:126820-126833` |
| Drain admitted deferred requests in order | `sqlite3.c:126921-126934` |
| Canonical global construction event order | `NormalizeValuesConstruction` |
| Original successful prepared root and cache publication | `PrepareConstructionValuesRow` |
| Existing first/incoming/fallback cohort decisions | `ConstructValuesRow`, `NativeValuesConstruction` |
| Per-query runtime row partition | `BoundValuesCore::chunks`, ADR-0222 |
| Common immutable source/expression owner | `BoundSelect::Impl`, ADR-0225 |
| Shared mutation transfer | `InitializeCommon`, `MoveCommon`, `query_owner()` |

`NormalizeValuesConstruction` already orders expression and VALUES-row
events by their source completion, then invokes the original constructor.
`PrepareConstructionValuesRow` binds each admitted root once and stores its
canonical `BoundExpressionId` in `SyntaxNormalization::construction_binding`.
Later real aliases/query copies reuse that binding.

The bound expression arena survives preparation, but `syntax_normalization_`
and `values_constructions_` are private binder workspaces. Published query
rows and chunks describe bound consumers, not all original early requests.
In particular, an outer normalized constant can omit the query while the
original constructor roots still exist in the common arena.

Distinguish binding from runtime reachability: `1 OR (VALUES...)` retains
its bound child, whereas an eligible `0 AND (VALUES...)` construction can
discard it. An incoming OR expression containing a query still fails the
native constant walk; the parser-folded AND control can remain eager.
Preserve both decisions rather than changing constructor eligibility to
fit an inferred root count.

Neither exact expression equality nor scanning every expression identifies
this missing ordered inventory. Rewalking syntax to reconstruct constructor
eligibility would duplicate the already authoritative construction algorithm.
Retaining another AST, cloning expressions, or treating every arena node as
a generated value would be incorrect.

## Decision

### Retain the successful original root requests

Add one statement-wide ordered vector of `BoundExpressionId` values to the
existing common `BoundSelect::Impl`. It records the successful original
early VALUES generation roots, in the order already established by
`PrepareConstructionValuesRow`.

Append an entry at the original successful preparation/cache-publication
boundary. Reusing an existing cached root does not append another request.
Do not reconstruct, sort, deduplicate by expression/value equality, or
collect descendants separately. The bound tree already owns each root's
children, original construction flags and token provenance.

Keep first-row/incoming-row decisions, fallback rows, width checking,
parameter assignment, names, collation/type-info work and all current error
boundaries unchanged. A consumer-filtered row does not become an early
request merely because it later binds successfully. An unused but genuinely
prepared early root remains in the inventory.

This vector is an ordered record of actual completed preparation requests,
not another expression cache, cohort partition, SQL-value container or
generation algorithm. The existing syntax-indexed cache continues serving
binding lookup, and the existing chunks continue describing runtime row
schedules. Those distinct roles are not replaced or conflated.

### Publish through the same preparation owner

Expose a borrowed `native_values_generation_roots()` span on `BoundSelect`,
following its existing immutable arena-access conventions. Every returned ID
addresses that same owner's `expressions()` arena.

Read and mutation compilation obtain the same span through their existing
common owner. Do not copy the inventory into every query, physical plan,
bytecode descriptor or VM. `MoveCommon` and bound-owner moves retain the
original owner and IDs; executable statements retain no AST or new runtime
state.

The inventory is statement-wide, not a per-query list of results. An ordinal
records request order; it is not permission to reuse a value by expression
identity, merge separate native creation sites, or share mutable coroutine
registers. Actual requested/reusable destinations, factoring-mode capture,
cohort sharing and invocation ownership remain separate generation decisions.

### Preserve boundedness and failures

Store only actual successful original roots. Do not reserve configured
maxima or speculatively allocate one record for every expression, query or
potential row. Use the existing checked container and allocation-error
patterns. Allocation failure unwinds the whole preparation through its
current typed boundary; no partial owner is published.

The inventory is populated during original construction, before later
speculative binding/rewind. Preserve that boundary and prove that published
IDs survive the existing alias and compound-order paths. Do not add a second
rewind algorithm or change the source-type work arena's lifetime.

Record a matched allocation/retained-storage baseline before implementation.
Distinguish the common owner's storage change from actual root-vector
allocation. Query-free and all-filtered cases must not allocate a root
buffer. Keep existing ceilings and rebuild measurement probes against each
version's consumed class definitions.

## Focused proof and bounded delivery

Start with an owning binder test for the published roots of a pruned eager
VALUES query. Then cover first/incoming order, restarted eager cohorts,
separate constructors whose preparation order differs from consumer order,
real alias reuse, all-filtered controls, owner moves and mutation common
ownership. Check root payloads/provenance and canonical IDs, not only counts.

Extend the existing complete allocation-failure sweep with an unused
constructor case. Preserve existing malformed-width/name/parameter and
speculative-binding tests without raising any limit. Current runtime rows,
callbacks, diagnostic winners, program resources and the eight new research
observations remain unchanged by this metadata-only prerequisite.

Independent design acceptance precedes production changes. Focused red/green,
complete fast units, implementation review and a normal source/ledger record
are required before a dependent slice. Keep the related prerequisite batch
reviewable under ADR-0106; a following generator slice requires its own
completed caller mapping and accepted design.

This decision does not activate constant-function factoring, a deferred
initializer queue, signed-token generation, error arbitration, shared eager
execution, IN query RHS or a later DAG node. Those behaviors and node43
remain incomplete.
