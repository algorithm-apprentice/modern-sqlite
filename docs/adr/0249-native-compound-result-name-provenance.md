# ADR-0249: Native Compound Result-Name Provenance

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #322 main `17d1bfc5`, ledger354
- Related contracts: ADR-0056/0092/0111/0143/0159/0192/0222/0247/0248

## Confirmed wrong results and completed ownership map

The singleton VALUES aggregate investigation exposed an earlier compound
ordering prerequisite:

```sql
VALUES(7,3)
UNION ALL SELECT 2,9 AS column1
ORDER BY column1;
```

Pinned SQLite orders by the second column, returning `(7,3),(2,9)`.
Both Modern facades instead match the first VALUES display label and return
`(2,9),(7,3)`. Output names and types agree; changing published metadata is
not the correction.

Forty-four retained paired observations distinguish eleven wrong bounded
row orders, seven native-invalid queries incorrectly accepted, one missing
later-row expression match, three separate alias-expression admission gaps,
and controls. The original three-column wildcard observation is malformed
against the four-column fixture; separate valid-width controls retain the
real wildcard behavior. Four-step protocols are bounded observations, not
complete-result evidence for larger joins.

| Responsibility | Canonical source |
| --- | --- |
| Name versus expression-span provenance | `sqlite3.c:20425-20474` |
| Simple result-name matching | `resolveAsName`, `sqlite3.c:111947-111970` |
| Local speculative resolution and structural comparison | `resolveOrderByTermToExprList`, `sqlite3.c:111989-112035` |
| Left-to-right native term search and output-column replacement | `resolveCompoundOrderBy`, `sqlite3.c:112057-112164` |
| Names inside larger expressions | `lookupName`, `sqlite3.c:111065-111130` |
| Coroutine wrapper versus ordinary VALUES terms | `sqlite3MultiValues`, `sqlite3.c:139901-140003` |
| Expanded wildcard name provenance | `sqlite3.c:155810-155852` |
| Existing result syntax, demand and name owners | `ResultDefinition`, `AliasEntry`, `CoreBindingMetadata` |
| Existing VALUES partition | `NativeValuesConstruction`, `BoundValuesCore::rows/chunks` |
| Canonical comparison and speculative cleanup | `NativeExpressionsEquivalent`, `BindingArenaCheckpoint` |
| Borrowed name context and real template copies | `NameScopeView`, ADR-0092 |
| Deferred source-type work | ADR-0192; `record_values_type_info_` |

Native `ENAME_NAME` is not a displayed column label. Explicit result aliases
and ordinary expanded wildcard names are eligible. An unaliased expression
instead needs successful resolution in the current term followed by structural
comparison. A double-quoted identifier that became a string does not acquire
alias provenance from its original spelling.

One Modern VALUES core can represent several native terms. Each ordinary
row is a term. Each eager cohort is one synthetic `SELECT *` term whose
expanded outputs have real names. Its producer expressions are not its
output-column expressions. A mixed core must therefore retain search order:
an ordinary row's DQS expression match can precede a later eager name, while
an unmatched ordinary row must not hide that later name.

The existing constructor and ADR-0222 publication already retain this exact
partition. No constructor reconstruction or new synthetic query is needed.
The comparator already preserves native literal, parameter, function and
collation identity. Its checkpoint retires speculative expressions, captures,
functions, queries and aggregate occurrences. Schema-check provenance remains
outside that rollback by its existing contract.

Native speculative comparison has the current term's sources, but no outer
name context. Modern currently retains the outer context and can incorrectly
accept both bare and qualified correlated ORDER expressions. Original result
binding and its legitimate captures are separate and must remain intact.

## Decision

### Preserve name provenance in the existing private definition

Add one name-eligibility fact to `ResultDefinition`. Set it for an explicit
result alias and for an expanded wildcard output. Leave ordinary expression
and direct VALUES definitions ineligible. Retain the fact through the same
definition ownership and query/template binding paths.

Use the existing result-column name only when that provenance permits name
matching. Remove the fallback that treats a result expression's identifier
or DQS spelling as an alias. Ordinary source-column names still resolve
through the canonical source resolver and structural comparator.

Do not change public column names, declared types, affinity, source ownership,
the alias registry used by other clauses, or any public carrier. Do not add a
second name table, AST annotation, namespace, expression cache or evaluator.

### Search the existing native output terms in order

For each current compound ORDER term, keep ordinal classification first.
Visit compound cores from left to right. A SELECT core contributes its
eligible names and existing output expressions.

Within VALUES, consume its already published rows, schedule and chunks.
An ordinary chunk contributes each row in source order. It has no eligible
result aliases, but its own expressions participate in canonical comparison.
An eager chunk contributes one named-output term. Match its existing
`columnN` names. If that fast path misses, retain the existing speculative
binding trial with an empty output-expression range. Its lookup diagnostics
still belong to preparation, but none of the original producer expressions
can become a comparison match. Parentheses and root COLLATE retain the
current normalization; unresolved likely wrappers do not become names.
Qualified eager outputs retain the existing comparison-context boundary
described below rather than acquiring a fabricated source in this slice.

The compact eager representation denotes one cohort; compact filtered rows
remain individual terms. Retained equal-schedule eager cohorts remain
distinct. Stop at the first successful native term/column match. Do not
promote every VALUES label to an alias, discard all eager names, or search
all names before earlier terms' expression matches.

Use borrowed views of existing storage, with no materialized term list.
Reacquire relocated owner objects after speculative nested binding; borrowed
row/result element storage must have a proven surviving owner. Retain current
row, expression and depth limits. Runtime VALUES visitation, generation
demand, producer ownership and aggregate programs do not change.

### Keep speculative binding local and transactional

Reuse `BindExpression` and `NativeExpressionsEquivalent`. Select a borrowed
comparison name scope with the current term's sources and no outer link.
Restore the prior scope on normal, typed-error and allocation exits. This
does not remove outer captures from the original result expressions.

Keep the canonical binding-arena checkpoint and source-type-recording
suppression. Failed name candidates may continue to a later term, as native
does; a final miss remains the typed compound-order error. Preserve actual
comparison failures and ordinary allocation unwinding, not a success-shaped
fallback. Successful trials also retire their speculative arena suffix.

An independently reviewed implementation correction closes a reproduced
regression in the first candidate: skipping an eager miss entirely lost
schema-retry requests. Binder provenance and warm-session I/O tests record
the actual red. Fourteen native/read/write warm-schema controls and fourteen
link-map-authenticated historical-source controls distinguish five lost
requests from pre-existing context differences. Restoring the canonical
trial recovers those requests without unconditionally setting a retry flag.
The historical control compiles the qualified-base binder with unchanged
headers; it is not the original main image or new main-CI evidence.

After selection, retain the existing result-column reference, explicit
COLLATE, null placement, aggregate-order-copy and lowering paths. No new
runtime instruction, sorter, register owner or query wrapper is introduced.

## Focused proof and bounded delivery

After independent design acceptance, add the original two-row wrong-order
regression and record its actual failure before production changes. Cover
explicit/quoted/case-insensitive/collated aliases, unaliased literals and
DQS values, wildcard versus qualified-column ambiguity, ordinary later rows,
single/mixed/restarted eager cohorts, and local versus outer comparison scope.
Include a later eligible term after a failed candidate and confirm original
correlation still executes through its real owner.

Use existing binder and lowering fixtures. Assert selected output indices,
unchanged metadata and complete small result sequences. Exercise source
lifetime, query copies, reset/rebind, typed rejection/recovery and both shared
facades. Add bounded scenarios to the existing complete allocation-prefix
owners without removing cases or changing ceilings, workers or deadlines.

The retained wrong-order protocols are the behavioral baseline. Non-target
ordinary, eager, wildcard, literal-identity and aggregate-ordering controls
must remain unchanged. Record the actual preparation/allocation effects and
verify that unchanged selected output columns retain their generated resource
shape. This is native resolution correctness, not a speed or heap-saving
claim; do not introduce a benchmark family or production instrumentation.

Run focused green, complete fast units, owning strict checks and the actual
change-aware closing plan. Preserve original failures and exact input
identities. Independent source/integrated review precedes normal squash;
fresh source-bound strict main-base protection and separate actual-main
qualification remain required.

## Explicit remaining boundaries

Three controls confirm the existing rejection of aliases inside larger
compound ORDER expressions, where native `NC_UEList` would resolve them.
That requires the canonical alias-copy context, not this whole-name
eligibility correction. Retain these native-valid gaps for the next bounded
comparison-context design rather than silently broadening alias visibility.

The warm-schema mapping also verifies a real eager source qualifier,
`"2-ROW VALUES CLAUSE".column1`, including `"*"` schema qualification but
not `main`. The qualified base rejects those otherwise valid references.
Its empty comparison namespace can also request unnecessary schema validation
for known eager names inside larger expressions. Native `NC_NoSelect` omits
nested SELECT name visits, whereas the existing full binding trial may request
validation from such a name. The historical-source executions establish these
as pre-existing gaps; retaining the trial does not claim to repair them.
Source expansion and post-name type information are separated at
`sqlite3.c:155409-155443,155925-156005`; generated source names are defined at
`33804-33835`, and non-descending resolution at `111835-111861,112635`.
Complete these related namespace and copy-context obligations through one
canonical comparison-context design before singleton aggregation or IN.

The native core-major loop also gives a later invalid ordinal priority over
an earlier unmatched ORDER term. The current term-major diagnostic difference
is retained separately; this slice does not claim complete error arbitration.
Query-bearing comparison, ordinary expression identity, source-type ordering
and special-function normalization remain governed by their existing owners.

Singleton/local VALUES aggregation, late omitted aggregate occurrences,
general value demand, fixed-factorable/FIFO initialization, original producer
generation and retained callback gaps remain open. No IN query RHS, node43
completion or later DAG node is admitted.
