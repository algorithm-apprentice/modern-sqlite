# ADR-0246: Retained Scalar Argument Generation Ownership

- Status: Accepted
- Date: 2026-11-10
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #319 main `a7b1bee8`, ledger342
- Related contracts: ADR-0036/0106/0111/0216/0218/0242/0245

## Requirement and completed bounded inventory

ADR-0245 makes scalar argument ranges follow actual generation demand.
The remaining binding is still cached by bound expression and physical
owner. Two emitted uses of the same scalar expression therefore share an
argument range even when its arguments require retained destinations.

Six fresh paired controls on qualified main342 preserve rows and the
registered diagnostic callback counts. The ordinary `max(id,7)` alias used
in both WHERE and projection emits two Modern calls over the same
two-register range. Pinned SQLite gives those calls distinct retained
ranges. Parameter and inline-constant arguments have the same distinction.
Both engines reuse scratch for the corresponding `max(id,id)` control.
Correlated alias instances already have distinct ranges; the existing shared
original VALUES definition contains one actual scalar call.

Current Modern register counts are 7 for the constant, nonconstant and
parameter aliases, 11 for the inline-constant alias, 22 for the correlated
alias and 25 for the shared original definition. Every measured one-less
register budget is rejected through the existing typed limit channel.
These are resource and fixed-work observations, not a SQL-result failure,
timing improvement or TDD red. The diagnostic callback counter does not count
the builtin `max` calls; their generated sites are inspected separately.

| Responsibility | Canonical source |
| --- | --- |
| Classify scalar constant arguments and allocate their ranges | `sqlite3.c:118295-118312` |
| Generate fixed-factorable argument values | `sqlite3.c:118335,118886-118921` |
| Retain constant-bearing ranges and release temporary ranges | `sqlite3.c:118362-118369,120520-120548` |
| Fixed deferred destinations and initialization order | `sqlite3.c:118698-118740,126922-126931` |
| Current bound-form constant traversal | `binder_detail::InspectConstantExpression` |
| Demand-owned main/private range selection | `ExpressionRegister`, `ArgumentBlock` |
| Checked late allocation and cached bindings | `EnsureExpressionRegister`, `ProgramBuilder::AllocateRegisters` |
| Concrete call range retained by the continuation | `BeginScalarEmission`, `EmissionFrame::first_argument` |
| Shared original generation and physical private owners | ADR-0218 and existing original VALUES once definitions |

## Verified pinned-reference defect

The pinned SQLite 3.54.0 snapshot dated October 2, 2026 uses a 32-bit
constant-argument mask as the decision between retained and reusable scalar
argument storage. Fixed-factorable generation still applies to every
argument, including arguments beyond index31.

With `items.id` values 2, 1 and 3, this admitted query exposes the boundary:

```sql
SELECT
  max(id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,111),
  max(id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,
      id,id,id,id,id,id,id,id,222)
FROM items;
```

Both the MSVC and GNU pinned libraries return `222,222` on every row,
whereas both current Modern session facades correctly return `111,222`.
The recorded native program gives both calls argument block3, then writes
111 and 222 into the same register35 during initialization before entering
the row loop. The later fixed initializer overwrites the earlier value.

Eight bounded cases establish the boundary: final constants at indices30
and31 work; constants after 32, 33, 63 or 64 dynamic arguments exhibit the
overwrite; a leading constant0 restores distinct native ranges and correct
results in the 32- and 64-dynamic-argument controls. The native protocols
match across both compiler profiles, and the two Modern facade protocols
match in all eight cases. Inputs remain unchanged and the owned facade
fixture is retired without mutation.

This is a defect in the pinned reference, not a newly introduced Modern
SQL-result defect or a claim about other SQLite versions. Do not copy the
incorrect result, narrow the admitted argument count, normalize away the
difference, or replace the pinned oracle to hide it.

## Decision

### Retain argument storage per actual generated call

For an actual scalar argument-range request, determine whether any argument
is constant using the existing canonical inspector in `Value` mode.
Inspect the complete admitted argument domain, not only the first32
arguments. This storage decision corresponds to native
`sqlite3ExprIsConstant`, not the separate effective-outer-ON exclusion in
`IsConstantNotJoin`, and does not depend on the caller's factoring mode.
Do not add another payload classifier or inspect only the entire call.

If any argument is constant, allocate a fresh checked contiguous range for
that actual generated call. Do not publish it in the expression-indexed
main or private argument cache. The existing emission frame retains the
concrete first register until the scalar instruction is emitted.
Distinct generation sites get distinct retained ranges even when they
share a bound expression ID and physical owner.

If no argument is constant, retain the current demand-owned scratch binding
within its actual main/private owner. Preserve zero-argument range semantics.
Do not add a temporary-register pool or a second range counter.

Use one canonical checked late expression-range allocator for fresh ranges
and the existing cached ensure path. The builder remains the sole owner of
emission-time growth. Preserve typed limit errors, checked width conversion,
allocation unwinding and recovery; allocation does not initialize a value.
The initial layout counter must not be used after the builder exists.

Keep expression homes, membership scratch, aggregate argument layouts,
symbols, constants and once results unchanged. A shared original definition
that emits one scalar call gets one retained range, not a range per alias
consumer. Generated-but-runtime-unreached calls still allocate; omitted
results still do not.

### Preserve correct full-width lifetime

The full-domain constant predicate is a narrowly scoped exception to the
pinned implementation's 32-bit storage decision. It preserves the intended
lifetime of fixed argument destinations and the already-correct Modern
results. It does not redefine a public auxiliary-data mask, add a VM flag,
change function metadata or claim native opcode/register-number identity.

This supersedes only ADR-0218/0245's cached scalar argument binding when the
actual call has a constant argument. Register counts can increase for
multiple generated retained sites, so configured exhaustion remains explicit.
No global resource limit, allocation ceiling, workload or deadline increases.
Storage growth remains linear in actual emitted argument requirements.

No value scheduling changes in this source. In particular, do not introduce
fixed-factorable requests, a deferred queue, general CodeTemp behavior,
original unused-producer generation or preparation-error arbitration.
The nine deferred-callback and two unrequested-constructor differences remain.

## Focused proof and delivery

Start from the qualified same-tree fast-unit and six-case resource baseline.
Before production changes, add a focused ownership regression showing that
the constant-bearing alias calls currently share their argument range.
Require disjoint retained ranges, ordinary result execution and exact-limit
rejection/recovery; retain a nonconstant scratch control.
This red specifies the new generation-ownership contract, not an invented
SQL-result regression.

Add focused full-width result controls around the first32-argument boundary.
Their independent expected maxima preserve the existing correct results;
keep the native mismatch as explicit evidence rather than rewriting the
oracle. Reuse existing private/query/original-alias, zero-argument, owned
TEXT/BLOB, read/write and reset/failure owners.

Run the complete existing lowerer allocation-prefix population. Its nested
constant-function cases already exercise the canonical continuation's
overflow storage; the additional classification walk must remain covered
at every allocation prefix. Record actual changed allocation/prefix counts,
not an assumed unchanged count. Retain all cases, workers and ceilings.

Repeat the six paired resource controls and eight full-width controls after
the change. Keep generated and executed work separate from register growth.
Use the existing matched native-contract baseline and actual selected closing
gates; do not introduce a benchmark family, instrumentation in the production
library, relaxed threshold or unmeasured speed claim.

Independent design acceptance precedes implementation. Focused green,
complete fast units, current strict compiler/checker owners and source review
precede the coherent node/ADR/ledger commit. Final exact-head local/hosted
and integrated review, normal squash and separate actual-main qualification
remain mandatory. Continue the remaining generation prerequisites before IN.
