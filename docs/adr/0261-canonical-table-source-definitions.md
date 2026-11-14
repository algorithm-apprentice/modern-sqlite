# ADR-0261: Canonical Table Source Definitions

- Status: Accepted
- Date: 2026-11-14
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #334 main `b53a9a36`, ledger410
- Related contracts: ADR-0106/0111/0252/0254/0255/0259

## Current boundary and selected increment

The source-preparation investigation retains its complete phase problem.
Fourteen fresh current-source controls contain eight error-owner differences,
three matching error controls and three exact positive protocols. Native
prepares nested sources before expression names, including through COLLATE;
function-local ORDER sources precede argument sources and FILTER follows
arguments. Missing wildcard diagnostics can be replaced by subsequent source
errors. These are source observations, not a completed preparation design.

The current `BindTableSource` combines two different responsibilities:
catalog/schema definition admission, and allocation/publication of an
executable binding source and its columns. The latter requires real bound
identities. It cannot be reused merely to obtain preparation metadata without
creating public query/column ownership obligations.

This increment separates those existing responsibilities without moving their
SQL phase or adding a preparation cache. It supplies a metadata-only admission
operation for the subsequent private-owner design and removes duplicated
catalog/schema publication setup. It does not pretend to fix source ordering
or `NC_NoSelect` by checking missing tables twice.

## Completed selected source map

| Responsibility | Existing owner |
|---|---|
| Native table admission before expression names | Pinned `selectExpander`, `sqlite3.c:155529-155629` |
| Native persistent table metadata lifetime | Catalog table reference retained by each `SrcItem`; source copies/cleanup at`sqlite3.c:114697-114818,131729-131759` |
| Immutable Modern catalog ownership | Existing `CatalogSnapshotPtr` in `StatementBinder` and the returned bound owner |
| Table spelling, schema check and aliases | `BindTableSource`, `NameParts`, `Dequote`, `IsSchemaTableName`, `NoSuchTable` |
| Real source identity and column publication | `AllocateSourceId`, `SourceBindingScope`, `BoundTableSource`, the existing source-column arena |
| Shared source lookup and enumeration | ADR-0254/0255; existing `FindSourceColumn` and namespace visitors |

`BindTableSource` already resolves a catalog `TableId` or the admitted schema
table before allocating a source ID. A quoted requested name can refer to
temporary decoded storage. The canonical resolved name must instead borrow
the retained catalog name or the static schema name. Alias text is already
owned and moved into `SourceState`.

The schema-table case and catalog-table case currently repeat source-ID
allocation, range initialization, source-state publication and table-source
publication. Their column payloads remain genuinely different: schema columns
have no catalog-column ID or rowid alias, while catalog columns preserve both.

## Decision

### One private resolved-definition value

Extract the existing metadata admission into `ResolveTableSourceDefinition`.
Return a private value containing the existing source kind, optional catalog
`TableId`, borrowed canonical name and owned optional alias. Use the existing
`BoundSourceKind` values; do not add a source-kind registry.

The only admitted combinations are `kCatalogTable` with a present `TableId`
and `kSchemaTable` with an absent `TableId`. Publication sets
`SourceState::schema_table` from the kind: false for the former, true for the
latter, never from a generalized absent-table test. ADR-0252's absent-table,
non-schema eager comparison state is explicitly outside this named-source
definition.

The value contains no `BoundQueryId`, `BoundSourceId`, expression ID, global
column range, SQL value or runtime cursor. A successful metadata lookup does
not allocate or publish an executable source.

Keep the existing name-part validation, admitted catalog schema, schema-table
aliases, dequoting order and catalog lookup unchanged. Reuse `NoSuchTable` so
schema-check requests remain a real lookup effect, not reconstructed from the
final error. Do not broaden metadata admission to derived FROM, views, table
functions or mutation-target correlation.

### Publish bindings once through the current owner

`BindTableSource` consumes the resolved value immediately at its current call
site. Allocate the real source ID and append `SourceState`/`BoundTableSource`
through one common path, using the correct column count and original source
span/join kind. Move the existing alias ownership once.

Retain the two existing column-payload loops. Preserve canonical names,
declared types, affinities, collations, catalog-column identities and rowid
aliases exactly. Do not introduce a second column container, copy catalog
metadata into an alternate namespace, or retain references across arena growth.

The definition borrows the catalog snapshot/static schema data only while
those owners remain alive. It must never borrow a temporary decoded identifier.
Keep source and column append order, reserve policy, checked source-ID
allocation, typed lookup errors and allocation unwinding unchanged.

### Keep subsequent phases separate

This is a behavior-preserving source-definition foundation. It does not
cache definitions, prepare another core early, change join/wildcard/error
ordering, suppress query descent, or publish incomplete queries.

The complete private prepared-source owner still must handle global pending
diagnostics, normalized expression traversal, compound conversion, generated
JOIN/wildcard metadata, template copies and discarded ORDER retirement.
The admitted definition value is not a substitute for that owner. Source
ordering, `NC_NoSelect`, IN and node43 remain incomplete.

## Green baseline and delivery

Use the accepted current C++ input/images and complete fast evidence as the
green baseline. Before the refactor, run the existing focused source/schema/
alias/join/copy/ownership controls. No artificial failing test is appropriate.

Afterward, run the same controls, complete fast tier, and existing binder
allocation/OOM owners without increasing ceilings. Retain every current
preparation protocol, including the known mismatches in the fourteen-case
baseline. Measure the same fixed-work allocation controls; no new allocation
or performance improvement is claimed.

Review this definition/borrowing design before implementation. The coherent
refactor requires implementation review and source/ADR ledger evidence before
any subsequent preparation-phase change. Apply ADR-0102/0106's actual closing
plan and delivery gates; do not combine an unaccepted phase change with this
green refactor.
