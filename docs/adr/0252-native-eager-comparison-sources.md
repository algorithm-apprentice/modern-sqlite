# ADR-0252: Native Eager Comparison Sources

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #325 main `e595e18a`, ledger366
- Related contracts: ADR-0092/0111/0143/0192/0222/0249/0250/0251

## Confirmed remaining namespace gap

Native eager VALUES cohorts expose a real ephemeral source to compound
ORDER comparison. Its name uses the cohort's row count:

```sql
VALUES(7,30),(8,20)
UNION ALL SELECT 2,40
ORDER BY "2-ROW VALUES CLAUSE".column2,1;
```

Pinned SQLite returns `(8,20),(7,30),(2,40)`. Modern rejects preparation
because ADR-0249's eager trial has an empty source namespace and no output
column expressions. Matching the original producer expressions instead
would be incorrect: the native outputs are columns of the ephemeral source.

The earlier fifteen namespace/type-phase observations and twenty additional
statement observations establish cohort-local names, repeated distinct
cohorts, qualified and `"*"`-schema lookup, ordering wrappers, preserved
outer captures, and the separation from nested source expansion. The newer
statement observations contain five native-valid preparation gaps, two
diagnostic-phase differences, and thirteen same-family rejection controls.
Raw diagnostic equality is not claimed.

Ten warm-schema observations inspect cached declared types before stepping
the candidate. Five namespace cases request unnecessary schema validation:
qualified whole names, `"*"` qualification, known bare columns inside
arithmetic/functions, and a known qualified source before an invalid ordinal.
Three controls retain real missing-name requests. Two raw-query cases expose
the separate `NC_NoSelect` gap and are not selected by this correction.

Existing ordinary VALUES rows remain separate native output terms. A
singleton ordinary row has no eager source. A restarted eager cohort includes
the preceding ordinary row and its new incoming row; native construction
starts its source row count at two. Equal-size cohorts remain distinct.
Use the retained partition, not a new classification of syntax.

## Completed selected source and ownership map

| Responsibility | Pinned or canonical owner |
| --- | --- |
| Eager source creation, restart and row count | `sqlite3MultiValues`, `sqlite3.c:139901-140003` |
| Generated source name | `sqlite3.c:33804-33835` |
| Ephemeral columns before source type information | `sqlite3ExpandSubquery`, `sqlite3.c:155409-155443` |
| `"*"` qualification and source-column lookup | `lookupName`, `sqlite3.c:110845-110918` |
| Local comparison and source-column identity | `sqlite3.c:111989-112035,119465-119563` |
| Later source type information | `sqlite3.c:155907-156005`; ADR-0192 |
| Existing cohort partition | `NativeValuesConstruction`, `ConstructValuesRow`, `BoundValuesCore::rows/chunks` |
| Canonical active source owner | `SourceState`, `source_states_`, `SaveQueryScope`, `RestoreQueryScope` |
| Existing column arena and leaf construction | `BoundSourceColumn`, `BindColumn`, `AppendExpression` |
| Canonical name and schema lookup | `ResolveNameInScope`, `ResolveSourceName`, `QualifiedSourceMatches`, `FindSourceColumn` |
| Existing ownership and captured dependencies | `OwnsActiveSource`, `OwnsAggregateSource`, `FinalizeQueryCaptures` |
| Structural identity and temporary retirement | `BoundSourceLeafIdentityOf`, `NativeExpressionsEquivalent`, `BindingArenaCheckpoint` |
| Completed output selection | ADR-0251 `CompoundOrderMatch` and real-key publication |

An ephemeral native column initially has no declared type, affinity or
collation and no visible rowid. Its source schema is absent; `"*"` can qualify
that source, whereas `main` and `temp` cannot. Producer/result metadata is
computed later and remains a separate obligation.

Modern already has the required private representation: a `SourceState`
without a catalog table and without `schema_table`. Existing noncatalog
column lookup scans the owned source-column range, and `RowIdAvailable`
already returns false for this state. Only source qualification currently
assumes every source belongs to the catalog schema.

The active source vector matters independently of a borrowed name view:
`RecordQueryCaptures` consults `OwnsActiveSource`, while aggregate definition
ownership can use `definition_name_scope_`. Installing only a borrowed
ephemeral source list would leave these owners inconsistent.

`BindColumn` uses the existing optional-collation interner and appends one
real column expression. The comparator already compares canonical source-leaf
identities. A checked contiguous range of these appended IDs can therefore
represent the ephemeral outputs without another result-expression vector,
alternate comparator, fake SQL value or executable query owner.

## Decision

### Activate one real cohort namespace through existing scratch

For an eager cohort that needs expression comparison, install its source in
the canonical active source scratch. Save the prior scratch and active ranges
through the existing query-scope mechanism; use the existing arena checkpoint
for temporary source IDs, columns, expressions and captured work.

Use the existing checked source-ID allocator. The private source has no
catalog table or schema-table flag, owns the native decimal row-count name,
and borrows column names from the current VALUES result-name owner. Its
column range contains the existing `BoundSourceColumn` representation with
absent declared types, `kNone` affinity, absent collation, no catalog-column
identity, no joined value and no rowid alias.

Document this private absent-table/non-schema invariant rather than adding
a second source-kind registry. Extend the canonical qualified-source match
only for that ephemeral state: accept an omitted schema or `"*"`, retaining
ordinary catalog/schema-table and alias behavior unchanged.

Do not add a public `BoundSourceKind`, `BoundTableSource`, synthetic
`BoundQuery`, catalog entry, parser wrapper or runtime cursor. This is the
binding context of the existing native cohort, not a new executable source.

### Materialize the binding view lazily once per visited cohort

Retain ADR-0251's encounter order, ordinal classification and eligible
whole-name fast path. Activate the ephemeral context only when the first
unfinished term in that cohort actually needs expression binding. Do not
inspect or decode later terms ahead of that point.

Once activated, reuse the same context for subsequent unfinished terms in
that cohort. Do not reconstruct it for every comparison. Each cohort owns a
separate temporary source lifetime, even when names and sizes are equal.
The canonical checkpoint may reuse an ID only after all earlier temporary
objects and references have retired. Never share a live context between
cohorts. Statements without an eager comparison retain the existing path.

Create actual output column expressions through `BindColumn` in the canonical
arena. Record their checked contiguous ID range and use a non-owning range
for the existing comparator. Each append is checked; no sentinel expression
ID or success-shaped placeholder is exposed. Do not retain a vector-element
reference across an append or nested binding that can relocate its owner.

The generated names remain owned once by existing result metadata. The
temporary source owns only its source name; no duplicated alias-name registry,
column-name vector, producer-value copy or second expression arena is added.

### Keep lookup, copies and retirement canonical

Pass the actual borrowed comparison scope to the existing transactional
expression matcher. Ordinary SELECT and ordinary VALUES contexts still come
from the same published-core scope helper. The eager scope supplies the
active ephemeral source while preserving the existing query/core identity,
comparison permissions and original lexical chain.

Fresh outward name lookup remains disabled. A valid original producer
capture or definition keeps its existing owner; the temporary source does
not expose fresh outer names or change query-template parentage. Active
source scratch and the borrowed comparison view must describe the same
source so nested query binding records real dependencies consistently.

Do not satisfy a comparison by provoking an internal ownership error. Do not
insert a fake published table/query owner to bypass capture validation.
Use the existing scoped query-binding suffix, aggregate registration,
name precedence and source-leaf identity.

Each expression trial retains its current checkpoint and source-type-work
suppression. The enclosing cohort scope retires its temporary columns,
source IDs and output expressions before leaving that cohort, including
typed failure and allocation unwinding. Restore the prior source scratch,
active ranges and name context in the correct lifetime order.

Checkpoint retirement removes temporary objects and IDs; vector capacity
continues to follow the existing canonical arena policy. Measure any changed
retained capacity explicitly rather than calling logical retirement a
zero-retained-byte result or adding an unrelated shrink/copy mechanism.

Schema-check requests remain outside arena rollback. Known ephemeral names
must not create requests, while real unknown columns, unavailable rowids
and later-core misses must retain their actual requests. Never reconstruct
these flags from the final error.

Only an output-column index escapes into ADR-0251's match slot. Real ordering
keys still reference the ordinary first public output owner after every
column has been selected. Preserve existing compound collation, NULL order,
aggregate-order copies, published types and row generation.

### Preserve the remaining preparation-phase boundary

Native comparison uses `NC_NoSelect`, while source expansion precedes name
resolution. Modern still binds nested queries during a comparison trial.
The mapped missing nested table therefore has a different diagnostic phase,
and two raw-query controls still request unnecessary schema validation.

This namespace correction does not introduce a second non-descending syntax
walker, fabricated scalar value, error-based shortcut or partial expansion
cache. The separate expansion/name-resolution owner must be mapped and
designed before that boundary is corrected. Existing nested trials must
remain structurally nonmatching and retire normally.

## Focused proof and delivery

After independent design acceptance, record the actual qualified-column
preparation failure before production changes. Assert selected output indices
and complete small native row sequences for plain, collated, `"*"`-qualified,
interleaved and repeated-cohort ordering.

Cover wrong cohort names, ordinary singletons, no visible rowid, disallowed
catalog schemas, unary/function/arithmetic nonmatches and the existing later
ordinal priority. Preserve later key/source-type collation errors rather than
claiming their current namespace miss is compatible.

Exercise original outer captures, scalar/EXISTS alias copies, moved owners,
reset/rebind and rejected-prepare recovery through both shared facades.
Assert no temporary source, column, query or expression identity survives
successful comparison or failure. Include nested query trials without
treating their known `NC_NoSelect` gap as repaired.

Extend the existing complete binder/lowerer allocation-prefix owners for
lazy activation, multiple keys, repeated cohorts and nested copies. Preserve
every previous case, ceiling, worker and deadline.

Record a matched successful-query allocation baseline for existing ordinary,
whole-name, ordinal, mixed-cohort and query-copy controls. Compare newly
admitted qualified queries with corresponding output-ordinal controls, while
reporting any arena-capacity difference rather than using failed preparation
as a performance baseline. This is namespace correctness, not optimization.

Run focused green, complete fast units, strict owning checks and the actual
change-aware closing plan. Independent source/integration review, protected
normal squash and separate actual-main qualification remain required.

This narrowly replaces ADR-0249/0251's empty eager comparison namespace and
output range. It does not change native construction, producer scheduling,
source type information, ordinary scope ownership or public runtime carriers.
Expansion/`NC_NoSelect`, later key-info/general error arbitration,
singleton/local VALUES aggregates, late clauses, remaining generation and IN
remain open. Node43 and the project are not complete.
