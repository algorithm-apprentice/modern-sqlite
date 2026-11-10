# ADR-0250: Canonical Result-Name Scopes

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #323 main `a27d5b1f`, ledger358
- Related contracts: ADR-0091/0092/0106/0111/0192/0249

## Confirmed result and schema-provenance gaps

The next compound comparison prerequisite has an observable wrong result:

```sql
SELECT 7 AS true,7+0
UNION ALL SELECT true+0,9
ORDER BY true+0;
```

Pinned SQLite selects the first core's second output and returns
`(7,7),(1,9)`. Both Modern facades omit that core's alias from the comparison
context, resolve the later boolean expression instead, and reverse the rows.
The corresponding `false` alias has the same selection defect.

Fifty-four retained paired observations distinguish two wrong row orders,
28 native-valid preparation gaps, four exact statement protocols and
20 both-reject observations. Three preparation gaps concern the separate
generated eager namespace; this result-definition slice selects the other
25. The observations retain raw diagnostics. Four-step traces are bounded
protocols, not complete-result evidence for larger joins.

Eight additional warm-schema observations inspect cached declared types
before stepping the candidate. Four known-alias nonmatches incorrectly
request schema validation: pure, function, query-containing, and
outer-correlated query-containing definitions. Three real missing-name
controls agree. The remaining raw-subquery `NC_NoSelect` difference belongs
to the later preparation-phase correction, not this slice.

The same canonical name owner also explains a noncompound defect:
`SELECT a.* FROM items a,items b ORDER BY id` is native-valid because the
expanded output name is eligible. Modern consults only its explicit-alias
list and instead reaches ambiguous source lookup. `ORDER BY id+0` remains
ambiguous in both engines: source lookup precedes aliases inside an
expression.

## Completed selected ownership map

| Responsibility | Pinned or current canonical source |
| --- | --- |
| Eligible result names and first name match | `sqlite3.c:111947-111970`; ADR-0249 |
| Source-before-alias lookup, scalarity, and resolved alias copy | `sqlite3.c:111065-111130,110495-110535` |
| Local compound comparison with `NC_UEList` | `sqlite3.c:111989-112035` |
| Native query-containing structural nonmatches | `sqlite3.c:119465-119563` |
| Aggregate argument source ownership | `sqlite3.c:111806-111833` |
| Original results and name eligibility | `BoundResultColumn`, `ResultDefinition::has_native_name` |
| Redundant current alias-name/target storage | `AliasEntry`, `aliases_`, `QueryBindingScope::aliases` |
| Per-core result/source ownership | `BoundSelectCore`, `CoreBindingMetadata` |
| Borrowed lexical names and real query copies | `NameScopeView`, `BindExpressionTemplateCopy`, ADR-0092 |
| Existing lookup and typed capture ownership | `ResolveNameInScope`, `ResolveOuterSourceName`, `BindAliasReference` |
| Speculative retirement and schema provenance | `BindingArenaCheckpoint`; ADR-0249 |

Native result expressions have already been resolved when an alias is copied.
A comparison's absent outer name context does not erase the copied
definition's source identities. Executed controls establish local aliases of
outer columns, outer aliases, quoted and boolean names, pure outer aggregates,
and a newly written aggregate over an alias of an outer column.

For example, native accepts an inner compound containing
`o.id AS x,sum(o.id)+0` and `ORDER BY sum(x)+0`. It still rejects the fresh
qualified spelling `ORDER BY sum(o.id)+0`. Existing resolved identities are
available through the alias; fresh outer name lookup is not.

Modern currently owns each explicit alias name twice: in the result column
and in a separate alias entry that also repeats its expression ID and result
index. Published cores retain the result columns and definitions, but
`ActivateQueryCore` clears the alias list. Copying another alias vector into
each core would preserve this duplication rather than establish the required
canonical owner.

## Decision

### One result-definition namespace

Remove the separate owning alias entry/vector. Borrow result columns and
their corresponding existing result definitions in `NameScopeView`.
Use one first-match eligible-name lookup for ordinary whole ORDER names,
compound whole names, and aliases inside expressions. Eligibility comes from
ADR-0249's existing provenance, not expression text or display labels.

Retain the generated eager whole-name policy already accepted by ADR-0249;
ordinary VALUES definitions do not become aliases. Do not introduce a second
name table, cached alias map, syntax traversal, or public metadata field.
The existing result-column names and expression IDs are authoritative.

The ordinary alias-reference binder receives a selected result index and
the same borrowed scope. It reads the target and definition from their
canonical owners, preserves aggregate/scalar checks, and uses the existing
pure-reference or query-containing template-copy path. First duplicate names,
source precedence, qualification, boolean fallback, DQS, and scope barriers
retain their native order.

Whole noncompound ORDER names now recognize genuine expanded wildcard names
through this same owner. Do not preserve the old defect by maintaining a
second explicit-only registry. Larger expressions continue to resolve
sources before aliases and keep real ambiguity errors.

### Borrow the actual active or published result owner

Ordinary binding borrows the existing scratch result columns and definitions.
Saved query scopes already own both vectors; moving them preserves their
element storage and therefore the borrowed view. No view survives its
binding scope or statement publication.

Published-core operations borrow that core's actual results, definitions,
sources and table sources. Centralize the existing per-core name-view
construction used by comparison, value activation and aggregate-order
copies instead of reconstructing separate namespaces. Retain the current
active-core and aggregate-phase bookkeeping.

Nested binding may relocate query/core metadata objects. Borrow vector
element storage only where its moved owner preserves it; reacquire owner
objects by index after nested binding. Existing expression, source, query,
depth and allocation bounds remain unchanged.

### Separate fresh lookup visibility from definition ownership

Keep a definition's original lexical outer chain available to canonical
template copying and typed source/aggregate ownership. Add one clause-scope
policy controlling whether fresh name lookup may continue outward; ordinary
scopes retain their current visible default.

Compound comparison enables the current core's result names but disables
fresh outward lookup. Current, qualified and outward name lookup use the
same visibility boundary. Preserve a selected definition-scope restriction
when nested expression binding changes its aggregate or value policy.

A real alias template copy selects the definition's original names and
ordinary outward visibility, while hiding its own result aliases as during
the original result binding. It does not see the consuming expression's
new sources or sibling aliases. The actual consuming query remains the
semantic parent of any new query instance under ADR-0092.

Do not apply the fresh-name restriction to ownership of source IDs already
imported through a valid alias. Those identities still have their original
lexical owner. The existing aggregate owner selection must retain that
relationship without performing new outer name lookup or inventing a source.
This is required by the captured-column aggregate controls, not permission
to expose a fresh `o.id` in the comparison.

### Retain one transactional comparison path

Use the existing expression binder, native structural comparator, scoped
query-binding suffix and binding-arena checkpoint. Suppress speculative
source-type recording as before. Retire speculative queries, expressions,
captures, functions and aggregate occurrences after both matches and misses.
No comparison query instance becomes runtime work.

Known query-containing aliases remain structural nonmatches. Their real
copies bind under the original definition scope, not the stripped fresh
lookup context. Preserve scalarity, inactive-value checks and ordinary
allocation unwinding through the existing copy mechanism. Do not substitute
an arbitrary value, fake query owner, diagnostic exception, or new walker.

Schema-check provenance remains outside arena rollback. A known alias
nonmatch must not become a missing-name request, while an actual later
missing name still requests validation. Use canonical lookup to establish
this; never clear or set schema flags from the final error shape.

Once a column is selected, retain the existing output reference, compound
collations, null order, aggregate-order bookkeeping, lowering and VM paths.
No runtime owner, instruction, sorter, register or public carrier changes.

## Focused proof and delivery

Record the actual two-row boolean-alias wrong-order failure before production
changes. Add owning selection cases for duplicate, quoted, parameter,
function, local/outer aggregate, captured and later-core aliases. Cover the
coupled wildcard whole-name correction and preserved expression ambiguity.

Keep explicit rejection controls for fresh outer names, qualified aliases,
nested aggregate misuse and nonmatching expressions. Exercise a real
query-containing alias trial before a later successful pure alias, original
outer-definition lookup, query/capture retirement, moved owners, reset/rebind,
and preparation recovery through both facades.

Add binder provenance and warm-session controls for the four known-alias
schema requests and real later missing-name requests. Retain the existing
14-case historical-qualified-base warm matrix; only newly specified context
repairs may change its outcomes. Preserve the unrelated raw-subquery and
generated eager namespace gaps explicitly.

Extend the existing complete binder/lowerer allocation-prefix owners with
the alias-copy and captured-owner paths. Keep all previous cases, limits,
workers and deadlines. Record matched preparation/allocation effects for
already successful controls and the accepted wrong-order baseline. For newly
accepted queries, compare retained resource shape with the corresponding
explicit output-column control rather than treating failed preparation as
a performance baseline. This is correctness, not a speed claim.

Run focused green, complete fast units, strict owning compiler checks and
the actual change-aware closing plan. Review design before implementation
and exact source before a normal coherent commit. Preserve source, integrated
and actual-main evidence, strict-main protection, normal squash and separate
MainQualified cleanup.

## Narrow supersession and remaining context work

This replaces only ADR-0091/0092's separate alias-list storage and
ADR-0249's disabled comparison-alias visibility. Their original definition,
semantic instance, capture, value, type-info and lifetime contracts remain.
ADR-0249's no-fresh-outer-name rule remains intact; its implementation no
longer severs the chain needed by a real copied definition.

This is the first bounded result-definition slice of the compound context
prerequisite, not its complete closure. Native generated eager sources need
their actual ephemeral namespace, including `"*"` qualification, no visible
rowid, and pre-type-info column metadata. `NC_NoSelect` needs the separate
source-expansion/name-resolution boundary: native expands nested tables
before comparison but does not resolve their column names there
(`sqlite3.c:155907-156005,110369-110389,112629-112656`).
Core-major matching must also preserve ordinal/error priority.

Those corrections require their own completed owner maps and accepted
buildable slices; this ADR does not authorize placeholder namespaces or
query values. They remain prerequisites before singleton/local VALUES
aggregation, general generation, IN, node43 completion or later DAG nodes.
