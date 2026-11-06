# ADR-0219: Native Invocation-Once Control

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #297 main `cc4d3c6a`, ledger244
- Related contracts: ADR-0031/0036, ADR-0106/0111 and ADR-0216/0218

## Completed mechanism and ownership inventory

The remaining generation owner needs invocation-once control attached to an
actual generated site. Ordinary function occurrences remain distinct, while
already generated eager VALUES code is shared through native alias copies.
Expression identity, coroutine activation and physical scratch ownership are
not interchangeable.

Current matched research retains65 preparation/value cases and their16 known
differences. An isolated native tracing derivative matches all65 unmodified
oracle outcomes, then81 outcomes including16 additional prepare-only
DML/collation callers. The actual WriteSession facade preserves the65
ReadSession controls. These are research observations, not a behavioral red,
new source acceptance or timing improvement.

ADR-0218 repairs a real private-scratch conflict without providing shared
once-result ownership. Its verifier/cancellation restrictions remain in force.
This source establishes only the native control primitive and consumes it in
the existing uncorrelated query cache. It does not activate ordinary constant
factoring or assign shared expression-result registers.

| Responsibility | Pinned or current source |
| --- | --- |
| A scalar function can request a separately allocated once result | `sqlite3.c:118259-118264,118698-118740` |
| Uncorrelated scalar/EXISTS subroutine gate | `sqlite3.c:116711-116760` |
| Per-invocation gate, marked before its body | `sqlite3.c:98945-98988` |
| Invocation epoch and bounded rollover | `sqlite3.c:105416-105500` |
| Native default epoch reset threshold | `sqlite3.c:24415` |
| Immutable executable programs and typed branch validation | `ProgramInput`, `ProgramBuilder`, `BytecodeProgram::Create` |
| Current result/ready register pair and query-call gate | `StatementLoweringState::subqueries`, `EmitSubqueryCacheInitialization`, `EmitBoundSubquery` |
| Successful query completion publishes ready | `EmitReadSubroutine` |
| Non-reentrant execution, row suspension, reset and failure cleanup | `Vm::Impl::Step`, `Reset`, `Fail`, `ClearExecution` |
| Independent coroutine activation/cancellation | `Vm::Impl::CoroutineContinuation`, ADR-0218 |

SQLite's top-level OP_Once compares a site epoch with the invocation epoch
held by OP_Init. Its program can self-modify those operands. Modern programs
are immutable and may be shared by independent VMs; execution state must
therefore remain in each VM.

Modern currently implements the uncorrelated query gate using an ordinary
SQL integer ready register, a conditional jump and a successful-completion
store. That existing consumer provides a bounded integration for the same
native mechanism. Do not retain both control implementations for that consumer.

## Decision

### One typed invocation-once primitive

Add a strong `OnceId`, an instruction `OnceInstruction` containing a site ID
and its already-seen target, and the corresponding instruction kind. The first
encounter in an invocation marks the site and falls through; subsequent
encounters jump to the target. Explicitly shared IDs share control state even
when instructions occur at different addresses. Different IDs remain distinct.

Add an initially zero site count to the existing program input and a checked
`ProgramBuilder::AddOnceSite` operation. Allocate IDs monotonically through
that same mutable builder. Add its label-aware `EmitOnce` counterpart using
the existing label/fixup implementation, not a second assembler.

Bound site growth by the unsigned32-bit identity range and the existing
instruction limit: every successfully published site population must fit
within the actual instruction population. Add no independent configurable
capacity or preallocated maximum. Rejected growth leaves the count unchanged;
a moved-from builder uses the existing invalid-builder error.

Validate site references and branch destinations at the existing immutable
publication boundary. Treat both successors conservatively in reachability,
definite initialization, resource/coroutine state and analysis-work accounting.
The branch must retain its physical instruction-region rules. The opcode does
not define a SQL register, initialize a result, grant a capture, or exempt a
write from coroutine ownership. An unwritten result behind a gate remains an
invalid program.

Keep existing resource and owned-byte checks. Measure the actual program,
instruction and runtime-state layout changes rather than assuming padding.
Programs without once sites retain no additional control-state allocation.

### Preserve the native epoch algorithm in VM-owned state

Use one per-VM array of unsigned32-bit site epochs and one invocation epoch.
These are execution-control numbers, not another SQL-value/register container
or a result cache. The array is sized from the verified site count during VM
creation and is not grown while stepping.

Advance the invocation epoch on the first Ready-to-Running step, never on a
row resume. At the pinned native threshold `0x7ffffffe`, clear site epochs,
restart the invocation epoch at zero and then increment it. Ordinary
invocations therefore do not clear every site. Use checked ordering that
cannot overflow the counter; add no public threshold override or test mode.

The once instruction compares/stores epochs and selects its successor.
It performs no allocation and no SQL-value conversion. Mark the site before
executing its body, as native does.

Reset, error retirement and context detach keep their current register,
cursor, temporary-storage and coroutine cleanup. They must not accidentally
replay an old once state: the next Ready execution advances the epoch.
Row suspension and coroutine reinitialization/cancellation do not start a
new statement invocation. Independent VMs over one immutable program have
independent site epochs.

Preserve the current Running-state rejection of reentrant Step/Reset.
An error or instruction-budget failure remains terminal until the existing
reset path; no retry or success-shaped fallback is introduced.

Keep the epoch transition in one small private VM helper over caller-owned
state so rollover can be tested at its real boundary without billions of
iterations, a reduced production threshold or a public test setter.

### Migrate the current uncorrelated query gate

Keep the existing statement-wide query bindings, result registers, read
subroutine descriptors, captures and lazy subroutine registration.
Replace the ready register in a query binding with an optional once-site ID.
Allocate that ID only when the existing uncaptured-query call path registers
its actual callable body.

Keep the current result-NULL initialization and every normal body result
assignment. Emit the once gate around the same existing subroutine call and
copy the same cached result afterward. Remove the old ready-zero
initialization and ready-one publication; do not keep a parallel ready flag.
The initial register layout reserves only the existing result register for
each query rather than an unused result/ready pair.

Captured queries continue through the existing inline path without a once
site. Do not reinterpret a captured-only expression as a statement constant.
Do not change query construction identity, name/value/width/collation demand,
native VALUES scheduling, scalar completion, temporary relation readiness or
read/write query ownership.

The same query-frame mechanism serves read and mutation hosts. No alternate
parser, query evaluator, fake query, registry snapshot or register pool is
added. Native function-generation modes and deferred expression queues remain
separate later work.

## Focused proof and bounded delivery

Begin with a focused bytecode/VM test using the new typed site and gate before
the interface exists, recording the genuine interface red. Then prove:

- first encounter, repeated encounters and explicitly shared versus distinct
  site IDs;
- row suspension, reset/rebind, error retirement and independent VMs sharing
  one immutable program;
- invalid site/target identities, label ownership, moved builders, rejected
  site growth and recovery;
- unchanged rejection of uninitialized values and illegal private-region
  control; and
- actual epoch rollover through the private transition helper.

Treat the lowerer migration as a behavior-preserving refactor over qualified
main244. Reuse owning scalar/EXISTS, nested and correlated query, read/write,
empty-result, reset/rebind, failure and allocation controls. Preserve all
existing stack/depth, allocation, model, watchdog and workload limits.
Include a focused negative control proving a captured query is not cached.

Record the actual instruction/register/control-state and allocation changes.
Existing matched function-call controls retain their rows, prepare behavior
and callback counts; the uncorrelated case remains once-per-invocation.
Do not claim the other ordinary or eager-alias callback differences are fixed.
Do not add a timing family or claim a speed improvement.

Run the complete fast tier and the actual change-aware closing plan, including
the escalation caused by public bytecode/VM contracts and private-header format
registration. Current selected fixed-work evidence must describe the actual
new instruction protocol; assertions, source work, thresholds and workloads
are not relaxed.

Independent design review precedes implementation. Independent source review
and a normal coherent commit/ledger entry precede a successor slice. Final
exact-head hosted/integrated/main gates and expected-OID cleanup still apply.

This narrowly replaces the existing uncorrelated-query ready-register
control. It does not authorize shared once-result register writes, an
initializer ownership exception, coroutine sharing, constructor rebinding,
ordinary constant-function activation or the broader generation scheduler.
The remaining name/value/scalar-width/collation/error-order, signed-alias,
reached-once/deferred and IN prerequisites remain open.
