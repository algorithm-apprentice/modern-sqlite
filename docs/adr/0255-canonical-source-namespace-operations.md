# ADR-0255: Canonical Source Namespace Operations

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #328 main `9e28fcd9`, ledger378
- Related contracts: ADR-0078/0080/0099/0106/0111/0252/0254

## Context and bounded requirement

ADR-0254 consolidated active and saved source ownership. The remaining
source-preparation investigation establishes that native source lookup,
join metadata and wildcard expansion precede expression-name resolution.
They cannot be replaced by a missing-table-only preflight.

The current wildcard implementation separately enumerates the same source
columns to count outputs and then to bind them. Its second loop embeds
generated-name resolution, joined-value selection and result publication.
USING contributor discovery similarly embeds bound-column construction
inside its namespace scan. These are concrete existing responsibilities
that a metadata-only preparation consumer must be able to reuse without
copying their algorithms.

This ADR selects only a green-baseline consolidation of those existing
operations. It does not introduce a prepared-query cache, private source
snapshot, new expression representation or preparation traversal. It does
not move namespace work to an earlier SQL phase.

## Completed selected native and current map

| Responsibility | Canonical owner |
| --- | --- |
| Native USING side lookup and ordered contributors | `sqlite3ProcessJoin`, `sqlite3.c:150020-150089` |
| Native generated equality and join provenance | `sqlite3.c:150086-150105` |
| Native wildcard source/column enumeration | `selectExpander`, `sqlite3.c:155635-155879` |
| Preserving-boundary generated qualification | ADR-0099; `WildcardUsesJoinedScope` |
| Current namespace ownership | ADR-0254 `SourceBindingScope` and `NameScopeView` |
| Physical and ephemeral column lookup | `FindSourceColumn`, `FindVisibleLeftColumn` |
| Current USING values and visibility | `BindUsingColumn`, `AppendJoinedCoalesce`, `BindJoinedColumns` |
| Current wildcard count and value loops | `ExpandWildcard` |
| Generated-name resolution and metadata | `ResolveSourceName`, `PopulateImplicitResultMetadata`, `JoinedValueMatches` |

`FindSourceColumn` uses the catalog's existing name index for catalog
sources and scans the actual column range for schema/ephemeral sources.
`FindVisibleLeftColumn` scans the current core's range and ignores merged
right contributors. Preserve those mechanisms and their existing bounds.

For a core containing RIGHT/FULL joins, USING visits later matching physical
left columns in source order. A nonmerged additional contributor produces
the current typed ambiguity error. Existing value construction starts with
the first physical left column and builds the ordered coalesce only when
additional contributors exist. Other cores retain their existing joined
left value. Do not change these distinctions.

Generated wildcard qualification is not equivalent to explicit physical
qualification. ADR-0099's later RIGHT/FULL and USING/NATURAL relationship,
owning-column spelling, selected value's metadata and duplicate-qualifier
behavior remain authoritative.

## Decision

### One wildcard column enumerator for both current passes

Extract one private, compile-time visitor operation that enumerates the
eligible source-column identities in their existing order. It reads the
already established source namespace and takes the existing optional
decoded wildcard qualification. Qualified wildcards include merged columns;
unqualified wildcards omit them.

Use this same operation for the current output-count pass and the current
binding/publication pass. Keep qualification decoding, source correspondence
checks, column-limit admission and their existing encounter order in the
caller. Do not resolve generated names while counting.

Extract the existing per-column binding/publication body into one helper.
It receives the real source ordinal and source-column identity, original
wildcard span, qualification presence and borrowed source syntax. It must
retain the same value-owner selection, generated qualification,
`ResolveSourceName`, `JoinedValueMatches`, implicit metadata and owning
output-name publication. No expression or query placeholder is introduced.

The visitor is an internal callback over existing metadata, not a plugin,
runtime strategy, new registry or materialized candidate list. It introduces
no owning container. The count callback increments the existing count; the
binding callback emits the existing real result and definition.

### Keep USING contributor discovery reusable without moving it

Extract the existing additional-left-contributor scan into a private
visitor operation. It performs the same name/visibility checks and visits
the same real source-column identities in order.

The current caller still binds the first left value before this scan.
Its callback retains the current lazy argument-vector construction and
per-contributor `BindColumn` calls. Keep the existing equality/coalesce
creation, ON accumulation, merged visibility update, RIGHT/FULL value
override and reported condition height after that scan.

Do not precollect another contributor vector, validate a later contributor
ahead of an earlier callback, reinterpret column IDs as expression IDs,
or change the current typed failure boundary. This extraction moves
algorithm ownership, not the observable SQL or allocation schedule.

### Make borrowed column storage explicit where required

Column-range helpers used by these operations should receive the existing
column arena as an immediate borrowed span rather than obtain a different
arena through hidden state. Keep catalog lookup canonical and use the same
span for schema/ephemeral range lookup.

Form spans at the immediate call site. Do not store a global column-arena
span in `NameScopeView` or another saved scope: nested binding can relocate
that arena. Visitor callbacks in this refactor may append expressions and
results but do not mutate source or column storage. Reacquire expression
objects by their existing IDs when an append may relocate their arena.

All current ordinary, joined, schema-table, eager, captured and copied
contexts must continue through these same helpers. There is no second
namespace for future preparation work.

## Explicitly unresolved preparation behavior

The qualified predecessor's32 context/18 warm observations remain open.
Thirteen additional traversal/construction observations and28 wildcard
observations establish further constraints, not new accepted behavior.
Every current protocol must remain unchanged in this refactor.

In particular, a core-local early-return wildcard correction is unsafe:

```sql
SELECT (SELECT 1 FROM absent_nested),wrong.* FROM t;
```

Both current engines report `absent_nested`. Native records a wildcard
diagnostic and continues source-query expansion, which can supersede it.
Returning the outer wildcard error before that traversal would regress the
matched control. Native also replaces earlier wildcard diagnostics, checks
the final expanded width afterward and aborts on that width error.
RIGHT/FULL USING ambiguity can likewise be followed by a later diagnostic.

Complete preparation therefore still needs an independently accepted private
metadata owner, explicit diagnostic/walk control and normalized native
query/expression order. Existing constructor errors must remain ahead of
source expansion; static compound-width front gates need their own entry
reconciliation. Do not create public orphan columns, fabricate queries or
values, clear schema-check requests, or infer native success from a final
error category.

This ADR does not close source expansion, `NC_NoSelect`, native core-name/
LIMIT ordering, later value arbitration, remaining generation, IN or node43.

## Green baseline and qualification

Authenticate qualified main378's complete fast-unit evidence and actual
native library/configuration. Run focused existing wildcard, preserving
join, contributor ambiguity, schema/ephemeral lookup, query-copy and moved-
owner controls before production edits. No artificial failing test is
required for this behavior-preserving refactor.

Afterward, run the focused controls and complete fast tier, preserving all
existing binder/lowerer allocation-prefix cases, ceilings, recovery checks
and deadlines. Compare the qualified30-case allocation/lifetime observer
with the same observer object and native profile; no new candidate list or
allocation is intended. Do not add trivial helper/accessor tests.

Retain exact joined rows, output names/types, source provenance and schema
request observations across both shared facades. Replay the current context,
traversal, wildcard and entry controls, including known differences; do not
relabel them as repairs.

Independent design acceptance precedes production changes. Source review
and complete per-slice evidence precede the normal commit. ADR-0106 governs
any subsequent related batch, whose next slice requires its own accepted
design. Final exact-head local/hosted gates and separate main qualification
remain mandatory.
