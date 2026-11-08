# ADR-0233: Original VALUES Generation Site Metadata

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #308 main `db2be97a`, ledger295
- Related contracts: ADR-0106/0111, ADR-0192/0222/0225/0229/0231/0232

## Current requirement and completed owner inventory

The remaining generation owner needs the original producer and destination
role of an early VALUES request, not just its bound expression. Native
construction assigns one result block to a producer, codes its first row
through SELECT's result-list contract, and codes incoming rows into the same
block through an expression list with flags0. Alias copies retain that
already-generated producer instead of creating another generation site.

ADR-0229 preserves every successful original root, including roots of a query
that a later parser-normalized expression discards. It deliberately publishes
only ordered expression IDs. ADR-0222's chunks describe bound consumer rows;
they cannot supply the missing producer information when no child query
survives. Neither expression-value equality nor adjacent root ordinals
reconstructs that information.

The existing interleaving control makes the distinction concrete:

```sql
VALUES(1),(2),(0 AND (VALUES(3),(4))),(5);
```

Original generation roots occur as `1,2,3,4,0,5`. The first two and last two
belong to one producer; the middle two belong to another. Conversely,
`VALUES(7),(8),(9 IS 9),(10)` has two distinct eager producers even though
their schedules are equal. A real alias retains the same prepared roots.
These are existing source/owning-test observations, not a new behavioral red.

| Responsibility | Canonical source |
| --- | --- |
| Create the original producer and its fixed output range | `sqlite3.c:139930-139979` |
| Generate incoming rows in that producer | `sqlite3.c:139983-140003` |
| Preserve generated addresses/registers during source duplication | `sqlite3.c:114697-114724` |
| Avoid regenerating an existing producer and initialize its scan | `sqlite3.c:157552-157560,166123-166133` |
| Original construction event order and cohort transitions | `NormalizeValuesConstruction`, `ConstructValuesRow` |
| Successful root/cache publication | `PrepareConstructionValuesRow` |
| One immutable root inventory and common read/write owner | `BoundSelect::Impl`, ADR-0225/0229 |
| Independent consumer partition and source-type work | ADR-0192/0222 |
| Existing checked typed identifiers | `BoundId`, bound expression/source allocation |

The current nine ordinary core scalars and admitted inline special functions
map to native CONSTANT declarations. Native can resolve those flags even
before an incoming expression has an `EP_ConstFunc` annotation. This does not
require a speculative slow-changing-function category or another function
registry. The present decision retains producer-site data only.

## Decision

### Enrich the existing original-root inventory

Replace the element type of the existing
`native_values_generation_roots` vector with a small
`BoundValuesGenerationRoot` record containing:

- the existing canonical `BoundExpressionId`;
- a statement-local `BoundValuesProducerId`, using the existing `BoundId`
  representation with its own tag;
- the original zero-based result-column position, using `std::size_t`; and
- an explicit `BoundValuesGenerationPhase` of `FirstRow` or `IncomingRow`.

The producer ID identifies an actual original constructor creation, not a
bound query, expression, value-equivalence class, coroutine activation or VM
register. Allocate it only when the existing constructor starts a new eager
cohort. Use one checked binder-local monotonic counter, not a source-span
hash, expression scan or second producer registry.

Replace `NativeValuesConstruction`'s current coroutine-presence boolean with
an optional producer ID. Its presence carries that same state. Keep a new
producer provisional locally until the original successful incoming-row
transition; fallback clears it at the same existing boundary. Do not retain
both a boolean and a competing identity state.

Pass the actual producer and column position into the existing row-preparation
helper. Publish a record at the same successful bind/root/cache boundary as
today, then publish the existing syntax-indexed cached expression ID. Reusing
that cached root does not append another record or relabel its original
producer, column or phase.

Preserve original event order, first-row/incoming eligibility, row-width
checks, fallback/restart behavior, parameter assignment, function flags,
name/collation/source-type work and every current failure boundary. An
incoming failure must not promote the constructor state or publish a
successful bound owner.

### Keep one owned representation and make the contract explicit

`BoundSelect::native_values_generation_roots()` returns a borrowed span of
these immutable records. This intentionally enriches that compiler-layer
interface from an expression-ID span to a record span. Update every existing
in-repository consumer to obtain its expression through the record's
`expression` member. Do not retain a duplicate ID vector, allocate an adapter
buffer or add a compatibility shim.

This narrowly supersedes ADR-0229's vector-element and accessor-return types.
Its actual-request ordering, successful publication, cached-root reuse,
common-owner lifetime and move guarantees remain unchanged. No executable
statement, public session result, persistent format, bytecode descriptor or VM
state gains this metadata.

Keep the existing bound query rows and `BoundValuesChunk` representation
unchanged. Those describe consumers; the enriched records describe original
generation requests, including unused ones. The source-type work arena keeps
its existing ownership and rewind. No AST is copied, reparsed or exposed to
the lowerer by this change.

Producer equality alone grants no permission to share mutable registers or
coroutine state. The remaining generator must still establish requested versus
returned destinations, caller factoring mode, retained-result ownership and
diagnostic ordering through the accepted architecture.

### Bound storage and preserve typed failures

Retain only the roots already admitted by the original constructor. Keep the
same vector growth policy and actual-root cardinality; reserve no configured
maximum. Query-free, singleton and entirely filtered cases still allocate no
root buffer. The record's larger element storage is an intentional measured
cost, not an unmeasured performance improvement.

Producer-ID overflow uses the existing checked bound-ID/error convention.
Allocation failures unwind through the existing typed preparation boundary.
No default producer, skipped error, alternate cache or partially successful
owner may be returned.

Record a matched preparation allocation/retained-storage baseline before
production changes. Rebuild observers against each version's actual consumed
headers: the return type changes even if the `BoundSelect` object's size does
not. Preserve all allocation/work/deadline limits. No new timing family or
production instrumentation is required.

## Focused proof and bounded delivery

After independent design acceptance, add one owning binder interface
regression for the distinct original producers retained after query pruning.
Record the missing-interface red before changing production.

Then cover first/incoming roles and multi-column positions, interleaved nested
producers, two equal-schedule cohorts, real alias reuse, entirely filtered and
singleton controls, and common read/write owner moves. Verify identities and
payloads, not merely record counts. Adapt existing root tests without changing
their previous expression, order or lifetime assertions.

Retain the complete owning allocation-failure sweep, including the existing
unused-constructor and multi-cohort/alias cases. Preserve malformed widths,
names, parameters, speculative binding and source-type tests. No existing
ceiling or failure expectation is relaxed.

Current SQL rows, preparation winners, callback counts and generated program
resources must remain unchanged. Reconcile or freshly execute the retained
115 protocol cases,103 read cases and eight callback cases using exact
source/image/consumed-definition evidence; the five known callback differences
remain open. Complete fast units, current compiler/checker owners, the actual
change-aware closing plan, independent implementation review and normal
node/ADR/source traceability remain mandatory.

This is one bounded original-site metadata prerequisite. It does not activate
constant factoring, a deferred queue, shared eager execution, signed-token or
error-arbitration changes, IN, or any later DAG node.
