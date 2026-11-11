# ADR-0254: Canonical Source Binding Scope

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #327 main `7e03bd93`, ledger374
- Related contracts: ADR-0090/0092/0106/0111/0192/0250/0251/0252

## Current ownership boundary

The remaining source-expansion and `NC_NoSelect` investigation has confirmed
fifteen expansion-phase differences and six extra/three lost schema-check
requests. Native prepares sources before resolving expression names. Modern
currently interleaves those responsibilities, and its joins and wildcards
also combine source metadata with expression construction and value checks.

This ADR does not repair or reclassify those differences. It defines one
behavior-preserving ownership refactor of the existing source state before
changing the preparation phases. It neither selects a partial table-lookup
preflight nor assumes that moving all existing source helpers earlier would
correctly implement native expansion.

The current active source state is split between `StatementBinder` and
`BoundSelect::Impl`: source names, the active column range and condition
height reside in the binder, while temporary table-source storage resides
in the eventual public query owner. `QueryBindingScope` repeats these
fields individually to preserve an outer scope while binding a child.
ADR-0252 already requires the active source state and borrowed name view to
agree; changing only the view misclassifies captured dependencies.

The necessary unit is already present, not a new cache: the two existing
source vectors and their existing range/height state. Consolidating that
unit gives active and saved scopes the same owner without introducing a
second namespace, public query, expression arena or source algorithm.

## Completed selected ownership map

| Responsibility | Pinned or current owner |
| --- | --- |
| Native source identity assignment | `sqlite3SrcListAssignCursors`, `sqlite3.c:131684-131701` |
| Native source-name, cursor and table-reference copying | `sqlite3SrcListDup`, `sqlite3.c:114697-114757` |
| Native clause/source copies and partial-failure cleanup | `sqlite3SelectDup`, `sqlite3.c:114776-114818` |
| Native source retirement | `sqlite3SrcListDelete`, `sqlite3.c:131729-131759` |
| Current source names and borrowed namespace | `SourceState`, `NameScopeView`, `CurrentNameScope` |
| Active/saved source lifetime | `QueryBindingScope`, `SaveQueryScope`, `RestoreQueryScope` |
| Catalog/schema and mutation-target creation | `BindTableSource`, `PublishMutationSource` |
| Joined columns and condition-height state | `BindUsingColumn`, `BindJoinedColumns`, `BeginQueryCore` |
| Published core ownership and reactivation | `FinishSelectQueryCore`, `CoreBindingMetadata`, `ActivateQueryCore` |
| Temporary eager source and checkpoint retirement | `MatchCompoundOrderByCore`, `RestoreBindingArena` |
| Real query-copy ownership | `BindSubquery`, `BindExpressionTemplateCopy` |

`BoundSelect::table_sources()` reads the first published core, not the
temporary vector in `Impl`. Mutation statements retain their actual target
and global source columns through the existing common query owner; they do
not expose that temporary vector through the public source accessor.

The planner's core loops use `BoundQueryView`, whose core range belongs to
a real query. Separately, `LogicalPlanBuilder::Build` plans every published
query identity, and mutation planning visits every published nested query.
An unresolved placeholder query is therefore not an alternative source
scope. Global public core/column arenas also remain public contracts, not
scratch storage for fabricated owners.

## Decision

### Aggregate the existing active source state

Introduce one private `SourceBindingScope` containing the existing
`SourceState` vector, `BoundTableSource` vector, active column begin/count
and accumulated join-condition height. Store it in `StatementBinder` and
as one member of `QueryBindingScope`.

Move the temporary table-source vector out of `BoundSelect::Impl`; do not
copy it or add another vector. Replace all direct accesses to the old
scattered fields with accesses through this single scope. Ordinary source
binding, joins, wildcards, aggregates, mutation targets, eager comparison
sources and source-ownership checks must use it consistently.

This is an aggregate of existing storage, not a polymorphic abstraction,
source-kind registry, new preparation traversal, allocator or cache.
Keep published `BoundTableSource`, `BoundSourceColumn`, `BoundSelectCore`
and query representations unchanged.

### Preserve moves, borrows and phase-specific state

`SaveQueryScope` must form its borrowed `NameScopeView` before moving the
source scope. Moving the existing standard-allocator vectors preserves the
element storage borrowed by that view. Restore the whole saved source
scope through the same RAII callers before its borrowed scope expires.
Do not replace a move with element copies or introduce raw owning pointers.

Keep all current clear/reserve/append operations and the same publication
points. `FinishSelectQueryCore` still transfers names to private core
metadata and table sources to the real public core. `ActivateQueryCore`
still copies the same existing fields from those owners. Do not reset
additional state simply because it now belongs to one aggregate.

The fields do not imply a new cardinality invariant. ADR-0252's eager
comparison has a real private source-name/column scope without an
executable `BoundTableSource`. Mutation target lookup uses its existing
per-source column range; the separate active SELECT range retains its
current initialization. Do not silently add an executable source or change
that initialization to make the aggregate appear uniform.

Retain source-ID allocation, column/expression arenas, alias-copy suffixes,
query captures, aggregate phases, source-type work and schema-check
provenance at their existing owners. Preserve checkpoint and scope-restore
ordering, including eager comparison failure and allocation unwinding.
The scope does not own or rewind the global arenas.

Temporary source vectors now retire with their binder scope rather than
remaining in a mutation's returned common owner. Their elements are not
borrowed by the published mutation contract. Retain the SELECT path's
existing explicit scratch-vector release point. Measure requested/live
allocation effects; do not describe this as a runtime optimization or
claim unchanged private allocation sizes.

### Keep subsequent behavior work explicit

This refactor changes no lookup order, source-expansion phase, wildcard
resolution, join construction, query descent or error priority. All thirty-
two current comparison protocols and eighteen warm-schema controls must
remain unchanged, including their known mismatches.

A subsequent accepted design must still separate complete source expansion
from name resolution, preserve normalized discarded subtrees and native
join/wildcard/copy cleanup, and represent non-descending comparison effects
without fabricated values or queries. Native right-to-left core-name/LIMIT
processing is a separate open boundary. This scope consolidation does not
admit IN, another DAG node or node-43 completion.

## Green baseline, verification and delivery

Use the qualified predecessor's exact production input and complete fast
unit evidence as the green baseline, authenticated against the actual local
library/configuration. Run focused existing ownership controls before
editing production code. Do not manufacture a red test for this refactor.

After the change, run the focused scope/capture/join/wildcard/eager/mutation
controls and complete fast units. Preserve the existing binder and lowerer
allocation-prefix populations, limits and recovery checks. Existing moved-
owner, nested-copy, rejected-prepare and session reset controls remain the
behavioral assertions; do not add tests of the aggregate's trivial layout.

Record a matched allocation/lifetime baseline for successful source-free,
catalog/schema, joined, copied-query, eager and mutation preparations.
Check both shared binding entry points where applicable, final retirement,
retained public identities and unchanged protocol/schema observations.
Keep instrumentation outside the production library.

Independent design acceptance precedes production edits. Source review,
complete per-slice evidence and node/ADR traceability precede the normal
commit. ADR-0106 governs any related slice batch; otherwise deliver this
coherent refactor independently. Final exact-head local/hosted gates and
separate main qualification remain required.
