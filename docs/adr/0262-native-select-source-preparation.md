# ADR-0262: Native SELECT Source Preparation

- Status: Accepted
- Date: 2026-11-14
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted ADR-0261 prefix `1f686f0`, ledger411
- Related contracts: ADR-0106/0111/0192/0252/0254/0255/0256/0259

## Selected observable boundary

Prepare the complete admitted SELECT source tree before expression-name
resolution. The fixed fourteen-case baseline contains eight error-owner
differences, three matching error controls and three exact successful
protocols. The eight differences cover nested sources versus names, COLLATE,
function ORDER/FILTER children, pending wildcard errors, compound width and
compound ORDER comparison. Closing these differences, not another ownership
refactor, is this slice's acceptance target.

This is not a missing-table preflight. Source admission, JOIN metadata,
wildcard expansion, pending diagnostics and native traversal must have one
private owner whose results are consumed by ordinary binding. Constructor
events remain earlier. General name/LIMIT/value ordering and `NC_NoSelect`
remain separate; a successful source expansion does not resolve those names.

## Completed source and existing-owner map

All native locations below refer to the manifest-pinned `sqlite3.c`.

| Responsibility | Pinned source / current owner |
| --- | --- |
| Expansion, names, then source types | `sqlite3SelectPrep:155992-156010`; `BindQueryWithSourceTypeInfo` |
| Compound conversion before expansion | `sqlite3SelectExpand:155907-155920`; `convertCompoundSelectToSubquery:155051-155112` |
| FROM admission and global pending-error guard | `selectExpander:155529-155629`; ADR-0261 definitions |
| JOIN names, contributors and pending errors | `tableAndColumnIndex:149819-149849`, `sqlite3ProcessJoin:149954-150124`; ADR-0255 helpers |
| Generated expression height and argument limits | `sqlite3ExprCheckHeight:113602-113613`, `sqlite3ExprSetHeightAndFlags:113682-113685`, `sqlite3ExprAnd:113955-113974`, `sqlite3ExprFunction:113981-114014`; existing validation helpers |
| Wildcards, replacement diagnostics, final width | `selectExpander:155635-155879`; ADR-0255 enumerator |
| SELECT and expression child order | `sqlite3WalkExprNN:110227-110259`, `sqlite3WalkSelectExpr:110296-110323`, `sqlite3WalkSelect:110369-110386` |
| Discarded zero-argument function ORDER | `sqlite3ExprAddFunctionOrderBy:114029-114070`; existing function argument classification |
| Source-copy and retirement intent | `sqlite3SrcListDup:114697-114757`, `sqlite3SelectDup:114776-114818`, `sqlite3SrcListDelete:131729-131759` |
| Constructor normalization and VALUES partition | `sqlite3MultiValues:139901-140004`; `SyntaxNormalization`, `NativeValuesConstruction`, ADR-0259 events |
| Actual identities, copies and rollback | `SourceBindingScope`, `CoreBindingMetadata`, `QueryBindingScope`, `BindingArenaCheckpoint` |

Native expansion assigns cursor identities but does not produce executable
queries. Immutable Modern syntax and public bound arenas cannot safely serve
as that scratch owner. Catalog metadata is already retained by the binder;
it must not be copied into a second column arena.

## Decision

### Private source metadata, not executable placeholders

Retain one prepared metadata entry per original syntax SELECT, including the
top-level SELECT. Index nested entries with existing syntax query identities,
not bound query IDs or repeated pointer searches. Size their outer storage
before construction can bind an incoming VALUES expression. Entered entries
are not successful entries until their expansion finishes.

Each core retains ADR-0261 table definitions, generated USING recipes, decoded
wildcard qualifiers and final result width. Definitions keep their exact
catalog/schema kind invariant and borrow canonical catalog/static names.
They own aliases. Column names and types remain in the catalog or static
schema definition; do not allocate a parallel column-name/visibility arena.

A USING recipe contains private source ordinals and source-local column
positions, its diagnostic span, and additional ordered contributors only
when native needs a coalesce. These coordinates are not public IDs. The
recipe itself records merged-column visibility. It contains no SQL value,
bound expression, query, cursor or alternate collation/function registry.

Move JOIN namespace admission into this owner; do not retain a second JOIN
lookup algorithm in value binding. Reuse catalog column lookup and move the
existing left/contributor scans to private positions. Generalize the existing
wildcard visitor to source-local positions and a borrowed visibility reader;
both metadata counting and actual value publication use that one enumerator.
Do not materialize all wildcard columns.

This narrowly extends ADR-0255's behavior-preserving extraction: the new
current requirement is phase-separated metadata, not a second source/value
implementation. No public representation or dependency edge changes.

### Match native expansion control, not a query-presence shortcut

Use the existing continuation-stack mechanism for an iterative depth-first
source walk. Enter each core by admitting all FROM definitions in order.
A real missing table aborts immediately and keeps its typed schema-check
effect. After FROM admission, an already pending diagnostic aborts before
JOIN processing, including a source-free core.

JOIN processing follows source order. Illegal NATURAL/ON/USING combinations
and absent USING columns abort. RIGHT/FULL additional-contributor ambiguity
sets a pending error: stop that contributor scan, but preserve native later
USING, generated-expression and wildcard work that can replace it. Skip
later contributor gathering when an error is already pending. NATURAL's
post-synthesis pending-error check remains a hard stop. Preserve actual
coalesce argument counts, accumulated WHERE/ON/equality height and the
existing typed limits; generated ON provenance prevents false-AND deletion.

Preparation checks construction height against the full construction limit,
not the caller's remaining name-resolution budget. Actual JOIN binding
retains its context-sensitive resolution check. If a contributor ambiguity
or argument-limit error is pending, native function-height finalization is
skipped: that coalesce retains initial height one and its equality has height
two, not the successful path's height three. Preserve this distinction when
later generated conjunctions approach the depth limit.

Expand the whole result list before expression walking. Qualified wildcard
misses and no-table wildcards replace the pending diagnostic and contribute
zero columns. Continue later result items. Only the final expanded width
check is a hard abort. Neither wildcard nor USING misses invent a schema
request. Never infer or clear schema effects from the final diagnostic.

After a core's expansion, visit result expressions, original WHERE, joined
ON expressions in source order, GROUP BY, HAVING, ORDER BY, LIMIT/offset,
then native prior compound cores. Generated USING predicates/wildcard
columns have no child SELECTs. Ordinary VALUES rows retain native right-to-
left term order. Use the retained constructor chunks: an eager cohort is
one source node, whose first producer was already prepared and whose
incoming expressions were consumed during construction. Apply the cohort
node's pending-error guard without walking those expressions again.
Never revive deleted incoming-row syntax or reclassify the chunks.

The first-producer event needs an explicit entry to this same source walker.
`PrepareConstructionValuesRow` currently binds expressions directly, so
the ordinary query-entry hook alone is insufficient. In its first-row
phase, prepare exactly that detached row before binding any row expression.
Do not mark or walk the containing SELECT's other rows, prior compound
chain, GROUP/HAVING, ORDER or LIMIT. Incoming-row phase retains its existing
non-descent. This distinction is observable for scalar function ORDER
children, which constant classification ignores: a missing source in the
first row precedes the scalar-modifier error, while an incoming row's
discarded modifier must not acquire a new source lookup.

Expression traversal uses native preorder and existing syntax children.
Function-local ORDER children precede arguments; FILTER follows arguments.
Zero-argument function ORDER is discarded. COLLATE is transparent to source
descent even though its native propagated subquery flag is absent. Reuse
constructor normalization to prune deleted expressions; do not revive their
queries, and do not use `has_subquery` as a traversal gate.

### Preserve converted and unconverted compound scopes

Any set operator in the chain plus propagated ORDER-expression COLLATE can
require native conversion; neither the last operator nor a root-only
COLLATE test is sufficient. Use normalized construction collation when
available, otherwise inspect the same ordinary propagation children without
descending into SELECTs or function modifiers.

For an unconverted compound, the rightmost core's ORDER/LIMIT children
precede its prior core. For a converted compound, enter the child compound
first, retaining its rightmost result/WHERE and all prior cores. Its
rightmost GROUP/HAVING and ORDER/LIMIT belong to the outer wrapper. After
the child finishes, apply the wrapper's pending-error guard, then visit
those outer clauses. Represent this traversal relationship privately;
do not fabricate a public wrapper query or change executable plans here.

Source expansion precedes the current compound-width front gate. Moving
remaining width checks relative to expression names is not claimed by this
slice. Likewise, source preparation occurs before compound ORDER trials,
but name-resolution suppression inside those trials remains `NC_NoSelect`
work.

### Consume once and retain correct lifetimes

`BindQueryWithSourceTypeInfo` obtains the prepared entry before ordinary
binding. Pass each prepared core to the existing source/JOIN/result paths.
`BindTableSource` publishes real source IDs and columns from its definition,
without another catalog admission. Resolve USING coordinates against those
actual sources and construct the existing equality/coalesce values. Emit
wildcards from retained qualifiers and the common visitor, not a second
width/admission pass. Keep current generated-name and joined-value semantics.

Original syntax entries can be shared by real query-template copies because
their metadata has no lexical value/capture identity. Each actual copy still
gets its own existing public source, query and expression ownership. Aliases
needed by a real binding are copied from the retained definition; this cost
must be measured, not described as allocation-neutral.

EXISTS still prepares ORDER child sources at the earlier expansion boundary.
Later discarded ORDER binding and arena rollback remain with their current
owners. Private metadata never extends a public query suffix or survives
the binder. Borrow only immutable catalog/static names; reacquire public
arena references after growth. Typed failure and allocation unwinding retire
all new storage without exposing partially prepared entries.

SELECT and writable nested-query entry points use the same owner. Do not
move mutation-target admission, add target correlation, or admit IN queries,
derived FROM, views, CTEs, windows or later DAG nodes.

## Proof and delivery

Before production changes, retain pinned controls for the eight differences
and record a focused owning test failure. Add focused coverage for pending
error replacement/guards, converted versus unconverted tails, JOIN metadata,
deleted syntax, zero-argument ORDER, final width, schema provenance and
template/EXISTS retirement. Pair first-producer and incoming-row function
ORDER controls and a detached-first-row versus future-core source control.
Cover mutation callers near the depth limit and pending coalesces immediately
below, at and above the generated JOIN-height boundary.
Successful joined rows, names and public owner
identities must remain unchanged through both shared facades.

Extend existing binder allocation-prefix cases for the new private-owner
paths, including failed preparation and copied queries; preserve previous
populations, limits and deadlines. Reuse the existing matched allocation/
lifetime observer and report additional retained/preparation work explicitly.
No runtime speedup or exact native allocation equality is claimed.

Run focused green, complete fast units and the actual final change-aware
plan. Obtain independent design acceptance before implementation and exact-
tree implementation acceptance before committing. Keep this behavior slice
in the same ADR-0106 batch as ledger411; final local/hosted gates qualify the
complete batch, not the metadata-only prefix. Node43 remains in progress.
