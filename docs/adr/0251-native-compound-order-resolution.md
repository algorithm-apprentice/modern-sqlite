# ADR-0251: Native Compound ORDER Resolution

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #324 main `05b98c7a`, ledger362
- Related contracts: ADR-0056/0092/0106/0111/0143/0192/0249/0250

## Confirmed resolution-phase mismatch

Native compound ordering visits output terms from left to right and tests
each still-unmatched ORDER term in its original position. Modern instead
finishes one ORDER term across all cores before starting the next, then
immediately publishes and value-checks that key.

For:

```sql
SELECT 7 AS x
UNION ALL SELECT 8
ORDER BY x+1,3;
```

Pinned SQLite rejects the second ORDER term as out of range. Its first
core resolves `x+1` without a name error, finds no output match, and then
rejects `3` before visiting the later core. Modern visits that later core,
reports the first term as unmatched, and requests unnecessary schema
validation because the later core has no `x`.

The second defect is the boundary between selecting output columns and
checking executable keys:

```sql
SELECT 1
UNION ALL SELECT 2
ORDER BY 1 COLLATE missing_collation,missing;
```

Native reports the second unmatched term and requests schema validation.
Modern reports the first key's collation error without reaching the real
missing-name lookup. This loses a required validation request, not only a
different diagnostic spelling.

Twenty-six paired statement observations distinguish thirteen selected
resolution/error-priority repairs, three separate later key-collation
arbitration gaps, six same-family rejection controls, and four exact
statement protocols. Raw diagnostics remain preserved; same-family errors
are not claimed byte-identical.

Eight warm-schema observations inspect cached declared types before any
candidate step. Two controls expose unnecessary later-core requests,
including an EXISTS consumer. One exposes the missing request behind an
early collation check. Five controls preserve real first-core/later-core
lookup requests and non-name failures.

## Completed native and canonical owner map

| Responsibility | Pinned or current source |
| --- | --- |
| Compound ORDER resolution after all result sets | `sqlite3.c:112548-112572` |
| Core-major unfinished-term loop and ordinal priority | `sqlite3.c:112057-112164` |
| Eligible names and local speculative comparison | `sqlite3.c:111947-112035`; ADR-0249/0250 |
| Keeping root COLLATE while replacing a selected term | `sqlite3.c:112126-112151` |
| Non-ALL collated-compound wrapper and resolution transfer | `sqlite3.c:155051-155119,112370-112384,112479-112499` |
| Later merge-key collation lookup | `multiSelectByMergeKeyInfo`, `sqlite3.c:152096-152123` |
| Key permutation from resolved columns | `sqlite3.c:152993-153022` |
| Registered collation errors | `sqlite3ExprCollSeq`, `sqlite3.c:113045-113107` |
| Canonical result names and lexical ownership | `NameScopeView`, `FindResultName`, `QueryCoreNameScope` |
| Current native output partition | `BoundSelectCore`, `BoundValuesCore::rows/chunks` |
| Speculative cleanup and schema provenance | `MatchCompoundOrderByExpressions`, `BindingArenaCheckpoint` |
| Existing integer and root-wrapper classification | `OrderByOrdinal`, `OrderByName`, `OrderByCore` |
| Existing final keys and aggregate copies | `BoundOrderingTerm`, `BindOrderByResultReference`, `BindCompoundAggregateOrderingCopies` |

Native stores mutable completion and selected-column facts beside its ORDER
syntax. The immutable Modern syntax cannot acquire those fields. Existing
published ordering terms also require real expression/collation identities;
they must not become success-shaped placeholders during name selection.

An ordinary VALUES row is one native output term. An eager cohort is one
named-output term, even when another cohort has the same schedule. The
existing retained partition supplies these boundaries; do not reconstruct
them from syntax, merge cohorts, or treat all rows as one candidate.

The first matched column remains independent for each ORDER term. A later
core may match an earlier key after the first core has already matched a
later key. The final key vector must keep SQL order, not match-discovery
order. Repeated keys remain repeated.

## Decision

### One bounded resolution-state owner

Use one private ORDER-term match-state vector, bounded by the existing
validated ORDER term limit. A slot records its optional selected output
column and, where needed, the existing decoded whole-name candidate.
It owns no SQL value, query, source, runtime register or second expression
arena.

Decode each whole-name candidate at its first native output-term visit,
retaining the current once-per-ORDER-term decoding behavior as visits become
interleaved. Keep integer classification and decoding in native encounter
order. Do not prevalidate all ordinals or decode later terms ahead of an
earlier term's real lookup. A valid ordinal is immediately resolved; an
invalid ordinal immediately returns the existing typed range error.

The match-state vector is the canonical mutable resolution state, not
another result-name registry or executable ordering representation. Its
separate lifetime is necessary because syntax is immutable and key
publication follows successful complete selection. Statements without
compound ordering keep an empty fast path.

### Visit native output terms before unfinished ORDER terms

Activate compound cores from left to right. Within each VALUES core, visit
existing ordinary rows and eager cohorts in their native order. For each
output term, visit still-unmatched ORDER terms in SQL order. Skip a slot
already resolved by an earlier output term.

Use the existing ordinal classifier, first eligible-name lookup and
canonical speculative expression binder/comparator. Preserve the current
SELECT/ordinary-row/eager-cohort distinctions. Eager misses retain their
existing empty-output comparison trial and diagnostic provenance; this
slice does not create the missing eager source namespace or match producer
expressions as output columns.

Stop once every ORDER term has a selected column. Otherwise continue to the
next native output term. After the final candidate, report the first
unmatched ORDER term using the existing typed compound-order diagnostic.
Do not return that diagnostic merely because an earlier ORDER term has no
match in the current core.

Use borrowed ranges of the existing rows, chunks, results and definitions.
There is no materialized native-term list, recursive core walk, restarted
constructor algorithm or new expression matcher. Keep existing depth,
source, row, expression, column and allocation limits.

### Keep selection trials transactional

Retain ADR-0250's current result-name scope and its distinction between fresh
lookup visibility and a definition's original lexical ownership. Real
query-containing copies use the same scoped query-binding cache and
canonical template mechanism. Preserve their structural nonmatch rule.

Each expression trial keeps its existing arena/capture/aggregate checkpoint
and source-type recording suppression. Successful and failed trials retire
the same temporary suffix. Reacquire relocated core/query objects by index;
borrow vector element storage only where moving its owner preserves it.

Schema-check provenance remains outside rollback. A lookup reached before
a later ordinal error must retain its request. A later core never visited
after that ordinal error must not create a request. Do not repair flags
from the final error code, clear a real earlier request, or add a special
per-name/per-error retry rule.

### Publish real keys only after every column is selected

After complete selection, append the existing ordering representation in
original SQL order. Use the selected column from the first public output
owner exactly as before. Retain existing root COLLATE wrappers, inherited
compound collation, sort direction, NULL placement and output metadata.

Only this phase performs the existing executable key/collation checks.
An unresolved later ORDER term or out-of-range ordinal therefore precedes
an earlier key's missing collation. EXISTS retains full name/range checking
while keeping its established inactive ordering-value policy.

Run the existing compound aggregate-order copy operation after real key
publication, exactly once. Keep original owner/core phases, immutable
targets, copied query identities and invocation finalization unchanged.
No parser, planner, lowering, VM, sorter, temporary format or public carrier
changes.

This reorders two existing responsibilities; it does not replace the
existing key-info/value validation mechanism with a new error accumulator.
Its separately observed last-collation and implicit-key behavior is outside
this bounded name-resolution correction.

## Focused proof and bounded delivery

After independent design acceptance, record an actual owning regression
for the wrong ordinal/compound-order diagnostic before production changes.
Cover first/second/third and signed ordinals, first-core misses, later-core
aliases, ordinary VALUES rows, eager cohorts, and EXISTS.

Add owning provenance and warm-session checks for both extra later-core
requests and the real request currently hidden behind collation checking.
Retain first-core missing-name, unknown-function, all-keys-resolved and
ordinary later-core controls. Error recovery must leave later preparations
usable through the shared read/write binder.

Assert selected output indices and complete small rows for keys matched in
different cores and repeated keys. Cover moved owners, real query-template
copies, aggregate ordering, reset/rebind and the absence of retained
speculative query/capture state. Do not validate only a returned row count.

Extend the existing complete binder/lowerer allocation-prefix owners with
the interleaved multi-key and query-copy paths. Keep every previous case,
ceiling, worker and deadline. The original behavior observations are the
error baseline. Record matched allocation effects for successful queries
with one/multiple keys, late matches and long names; verify retained public
and generated-resource shapes. This is correctness, not an optimization or
a native-malloc/timing claim.

Run focused green, complete fast units, strict owning checks and the actual
change-aware closing plan. Review exact source before a normal coherent
commit with node/ADR/ledger traceability. Final source-bound hosted,
integrated, protected-squash and separate actual-main gates remain required.

## Explicit remaining boundaries

Three additional observations show that multiple missing explicit or
inherited key collations select the last native error while Modern retains
the first. `multiSelectByMergeKeyInfo` and the later duplicate-key operation
can also visit implicit non-ALL keys. That is a separate key-info/error
arbitration owner; do not implement a partial last-error loop only for
explicit ORDER keys here.

The current result-core binding order, LIMIT phase and general value-demand
arbitration are not corrected by changing the compound ORDER matcher.
Do not claim whole-statement error-priority parity.

Generated eager-source namespaces and source expansion versus `NC_NoSelect`
remain required context work. The existing eager empty-scope/schema
differences remain explicit. Singleton/local VALUES aggregates, late clauses,
general logical/deferred/FIFO/original-producer generation and IN follow
their remaining prerequisites. Node43 and the project stay incomplete.
