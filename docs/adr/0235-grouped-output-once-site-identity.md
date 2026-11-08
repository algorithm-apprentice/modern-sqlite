# ADR-0235: Grouped Output Once-Site Identity

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source300 `69d56dc6`, ADR-0234
- Related contracts: ADR-0106/0111/0218/0219/0231/0234

## Current requirement and completed source mapping

Source300 closes the five original ordinary constant-function callback
differences through native reached-once control. Its wider contextual
observation deliberately retains two grouped-output differences:

- a constant result grouped by another key: native1/Modern2 callbacks; and
- a nested constant-function result: native3/Modern5 callbacks.

All fourteen contextual queries retain equal rows and no preparation
callbacks. The failed all-context comparison is preserved. Source300 is an
accepted local prefix, not merge authorization; this correction is required
in the same related batch before delivery.

Native creates one group-output subroutine, including HAVING and result
generation. Both a group-boundary call and the final-group call enter that
same definition. Its function once sites therefore do not multiply when
output is requested from another control edge.

Modern's existing grouped control flow physically emits that output twice
through one compiler callback. Assigning a fresh gate at each physical
emission, as source300 does, does not yet retain this native logical identity.
This is not evidence that equal expressions anywhere should share a result.

| Responsibility | Canonical source |
| --- | --- |
| Group-change call to the output definition | `sqlite3.c:158251-158255` |
| Final-group call to the same definition | `sqlite3.c:158279-158282` |
| Single generated HAVING/output body | `sqlite3.c:158299-158310` |
| Existing group-change and final callbacks | `grouped_aggregate_lowering.cpp:338-343,464-469` |
| Canonical completed-group output | `PlanLowerer::EmitCompletedGroupedAggregate` |
| Representative selection and output context | `RestoreAggregateContext`, `ReadRegisterLayout` |
| Explicit shared once IDs at distinct addresses | ADR-0219, `OnceInstruction` |
| Physical register owner and contiguous definitions | ADR-0218, `ExpressionRegisterOwner` |
| Fresh retained result and protected construction | ADR-0234, `BeginReachedOnceEmission` |

The existing once primitive explicitly permits several instructions to use
one site ID. It does not initialize their result, grant a capture or relax
write ownership. No new VM encoding or callable kind is needed.

## Decision

### Identify one concrete grouped-output definition

Retain a compiler-local definition record for every actually visited grouped
output definition in the owning query frame. Its key is the actual aggregate
program, read-register layout, physical `ExpressionRegisterOwner` and incoming
factoring mode. It is not an expression-equivalence key or a runtime cache.

A different key selects a different record; it must not replace the previous
one. A nested compound can generate definitions A, B, A, B across the two
enclosing output copies. Parent-owned reproduction confirms that replacing
the current record loses A, even when B has no once events. Retain both
event-free and eventful definitions until the owning compiler frame retires.

Use the existing query-frame ownership pattern: one vector of uniquely owned
definition records, populated only for actual definitions. Record addresses
must remain stable when that registry grows, because an enclosing traversal
may still borrow a record. Do not retain a statement-wide scope-by-expression
matrix or reuse a record across different physical owners or factoring modes.

Reuse the whole-statement potential-root check already required by invocation
value initialization. When that immutable arena contains no potentially
reached function root, bypass grouped once bookkeeping entirely and preserve
the existing no-function allocation path. Compute this summary once, through
the same predicate as the initializer. This is not permission to discard an
event-free B when A can produce once events: that statement's summary is true,
so both definitions retain the stable ownership above.

At `EmitCompletedGroupedAggregate`, establish one scoped, ordered traversal
of reached-once generation events. The first emission records each admitted
event in actual continuation order. A later physical copy of that same output
definition replays the recorded sequence.

Each event retains the bound expression identity used at that position, the
existing `OnceId` and its retained result register. Repeated occurrences of
the same expression still occupy different sequence positions. HAVING,
projection and ordering sites are not deduplicated by expression or value.

On replay, require the next event's identity and final event count to match.
A mismatch returns the existing typed internal-invariant error, never a new
site, a stale register or an invented successful result. A failed compilation
publishes no program.

### Reuse only the proven once product

Extract source300's fresh site/result allocation into one canonical helper.
Outside the active grouped traversal it keeps its current behavior. During
the first grouped emission, allocate and record through that same helper.
During replay, reuse the recorded site and retained result instead.

Still emit the inline gate, its disabled-factoring body and the consumer copy
at both physical locations. Each copy uses its own existing labels and caller
destination. The common site state ensures that only the first reached body
executes in an invocation, whichever physical location is reached first.
The second location must work when it is the only one reached.

Initialize the shared retained result exactly once through the existing
protected entry mechanism. Do not append a second constructor on replay or
turn the result into a moved coroutine output. Keep all existing private
register, capability, initialization and once-epoch checks.

This preserves the native single-definition once identity through Modern's
existing two physical control paths. It does not change grouped input,
finalization/reset order, representative selection, HAVING/offset/limit
behavior, output publication or cleanup. It does not move preparation
operations to a later definition or introduce another evaluator.

### Scope nested generation and failure cleanup explicitly

The active traversal is a borrowed pointer to an emission-local object and is
restored by the existing RAII style on every exit. Keep its borrowed parent
link so reentry of an already active definition is rejected throughout the
active traversal chain, not only at its top. Nested grouped output uses its
own stable definition record and restores the enclosing traversal afterward.
Ordinary nested expression generation stays in its actual enclosing logical
output definition; unrelated delayed private definitions do not inherit an
expired traversal.

Use the common bound arena's IDs, checked builder allocation, existing result
initialization and current typed failure boundary. Keep storage proportional
to actual admitted sites. Empty site populations retain no site-buffer
allocation; do not reserve configured maxima.

Record the compiler-state and retained-program allocation cost, including
non-grouped controls if the containing compiler object's size changes.
Do not hide that cost or claim a speed improvement. Allocation failure retires
all partially collected metadata through the normal owner.

## Focused proof and bounded delivery

After independent design acceptance, add an owning regression for the
constant result grouped by another key. Record source300's two-versus-one
failure before changing production.

Cover the nested-result difference, two independent same-valued occurrences,
multiple grouped cores/private producers, a final-group-only invocation,
empty input and unreached output. Preserve grouped representative reuse and
the disabled IN caller mode. Verify scalar errors, reset while output is
suspended, independent VMs and owned TEXT/BLOB results using generated
grouped programs.

Combine nesting with multiple grouped cores: an event-free second compound
arm and an eventful second arm must both preserve the first arm's products
through enclosing replay. Keep the originally accepted single-record design,
its implementation review finding and both actual reproductions as superseded
evidence; do not describe the earlier successful individual controls as proof
of this combined lifetime.

Extend the existing owning allocation-failure population with a grouped
output that records and replays multiple once products. Keep every existing
allocation ceiling, worker count, workload and deadline.

The two implementation sources expand the owning population from167 to170.
Keep the fixed-population assertion in `CiWorkflowPolicyTest` synchronized
with that complete fixture set. A separate corrective source updates its
stale167 expectation to170; it must not remove the assertion, reduce coverage,
change the two-worker ownership contract or relax any existing limit.

The no-function allocation owner's observed58-to60 regression must return to
its existing58 expectation through this demand-only metadata allocation,
not by changing the assertion. Its other exact allocation controls remain
unchanged.

Complete the fourteen paired contextual cases and retain the original eight
callback cases. The115 native/write and103 read preparation protocols remain
unchanged. Measure actual site/register/instruction, allocation and retirement
deltas against accepted source300, with rebuilt, definition-matched observers.

Run focused red/green, complete fast units, the actual compiler/checker
owners, independent implementation/source review and normal ledger
traceability before committing this dependent source. Final integration,
hosted and actual-main qualification apply to the exact related-source batch
head, including the CI population correction.

This narrowly supersedes ADR-0234's physical-emission site identity for the
same proven grouped-output definition. Original VALUES cross-consumer
generation, general deferred/reusable requests, preparation diagnostic order,
signed generation, IN query RHS and later DAG nodes remain separate work.
