# ADR-0230: Owned Collation Availability for Bound Symbols

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source282, PR #306 head `4b9ba9f`, ledger282
- Qualified delivery base: PR #305 main `10579bcd`, ledger281
- Related contracts: ADR-0033/0052/0089/0106/0111/0192/0225/0229

## Current requirement and completed inventory

The remaining value-generation owner must distinguish an interned collation
name from a registered comparator without retaining the synchronous binding
environment. Missing-collation diagnostics still belong to their actual
generation/source-type demand, not to name interning.

The current binder already owns a registered-name snapshot for planning.
`RegisterActiveSourceCollations` populates it only for joined scopes or a
source with a stored index. Actual source282 inspection confirms that
source-free scalar/comparison queries, a single unindexed source and unused
early VALUES roots can publish collation IDs while that snapshot stays empty.
An unused missing name is also interned without being registered. A joined
control publishes the actual three core registrations.

The seven current metadata controls are read-only research. They do not prove
a runtime collation defect or repair the retained native value/collation
diagnostic-order differences. This source supplies the missing ownership
invariant for those later decisions.

| Responsibility | Pinned or current source |
| --- | --- |
| Select a column or explicit expression collation | `sqlite3.c:113045-113110` |
| Look up a registered descriptor without requiring it | `sqlite3.c:132731-132748` |
| Resolve a demanded comparator and report absence | `sqlite3.c:132777-132807` |
| Validate immutable synchronous binding metadata | ADR-0033; `ValidateEnvironment`, `BindEnvironment` |
| Current owned-name publication | `RegisterActiveSourceCollations` |
| Canonical used-name IDs | `InternCollation`, `BoundSelect::Impl::collations` |
| Canonical required-collation diagnostic | `RequireRegisteredCollation`, ADR-0192 |
| Existing planner consumer | `IsRegisteredCollation`, `IsSupportedIndexShape` |
| Shared read/write lifetime | `MoveCommon`, `query_owner()`, ADR-0225 |

SQLite's connection-owned `CollSeq` lookup and later requirement are distinct.
Modern's accepted immutable boundary copies metadata and registration
generation; it must not retain `Collation*`, a registry span or an environment
pointer. Native factory callbacks and encoding substitution are not added by
this metadata prerequisite.

## Decision

### Complete the existing snapshot when a bound symbol needs it

Keep the existing `registered_collations` vector and public immutable view.
Extract its existing reserve-and-copy operation into one private, idempotent
retention helper. Preserve environment order, spelling and the existing
case-insensitive name semantics.

Call that helper from the existing indexed/joined-source admission and
whenever `InternCollation` publishes or returns a used-name ID. The common
owner therefore has the complete immutable registered-name snapshot whenever
it contains any interned collation, or whenever existing index planning
requires the registry.

An empty snapshot remains valid when neither role needs it. In particular,
a source-free numeric literal with no collation ID need not allocate the
snapshot. Do not interpret that empty view as permission to assume arbitrary
collation availability.

`ValidateEnvironment` already requires a non-null BINARY registration and
rejects null descriptors before common initialization. Thus a successfully
retained registry is nonempty; use the existing empty-vector state instead
of adding a completion flag or another registry.

Do not add an availability bit to every `BoundCollation`, a second name set,
a hash cache, callback interface or a separately copied query registry.
Referenced-name IDs and the existing registered-name snapshot have different
roles; an unknown but unused name must remain representable.

### Preserve lookup, demand and error timing

Retaining names does not resolve or validate the requested collation.
`SELECT 1 COLLATE missing` must keep its current successful binding, while
an actual required comparison continues through the existing typed
`RequireRegisteredCollation` kernel at its original point.

Keep `RequireCollation`'s value-demand guard and `value_requirement` marker.
Keep unconditional VALUES source-type checks and their ordered last-error
handling. Do not move a missing-collation error ahead of name resolution,
numeric generation or an existing source-type boundary.

The existing synchronous environment lookup remains authoritative for current
validation. This change only makes the same immutable name inventory
available to later preparation consumers. It does not activate a later
generator, alter native expression identity, choose a comparator, invoke a
callback or change the VM's runtime symbol resolution.

### Keep one common owner and existing rewind

Store names once in the existing statement-wide `BoundSelect::Impl`.
Nested and copied query views use that owner; INSERT, UPDATE and DELETE use
the current common-owner transfer. No names are copied into physical plans
or bytecode beyond their existing contracts.

Use the existing `BindingArenaCheckpoint::registered_collations` size and
rewind. If speculative binding retires a newly created snapshot, the next
real symbol demand repopulates it through the same helper. Do not add a
second rollback mechanism or leave a stale initialized flag.

Preserve the accepted registration-generation invalidation rule. Owned names
remain usable after the binding environment's pointer array is retired, but
do not permit executing a program against changed descriptor semantics
without the required reprepare.

### Bound and measure the retained cost

Copy only the actual supplied registry, at the existing or newly required
demand point, and at most once in the retained common owner. Do not reserve
configured maxima or duplicate the registry per expression/query.
Preserve all allocation/error ceilings. Allocation failure unwinds the
current preparation; no partial owner or successful default is published.

Record a matched baseline before implementation. The accepted source282
measurement may be reconciled only if probe source, flags, consumed headers
and library identities still match; otherwise run a fresh baseline.
Distinguish requested, live and peak bytes and check owner retirement.
No new timing family or speed claim is required.

## Focused proof and ordered delivery

Start with an owning binder regression that interns a source-free comparison
collation but observes no registered-name snapshot in the current version.
Record the actual pre-change assertion failure.

Cover known and unknown unused names, required missing-name diagnostics,
custom registration subsets and order, single unindexed sources, retained
early roots with no bound child, real aliases, speculative compound trials,
common-owner moves and mutation hosts. Retain the existing indexed/joined
controls. Update the old single-source empty-snapshot expectation only for
the explicitly superseded symbol-demand contract; keep the no-symbol
empty-snapshot control.

Extend the existing complete allocation-failure sweep with a source-free
unused-name case. Keep all existing tests and limits. Current rows, callback
counts, diagnostic winners, root inventories, program resources and
reference-control protocols must remain unchanged.

Independent design acceptance precedes production changes. Focused red/green,
complete fast units, implementation review and a predecessor-linked normal
source commit remain required. This is a related preparation-owner slice
after source282 in PR #306 under ADR-0106, not permission to skip final
exact-head local, hosted, integrated or actual-main qualification.

The general native generator, diagnostic arbitration, signed generation,
IN query RHS, node43 and later DAG nodes remain incomplete.
