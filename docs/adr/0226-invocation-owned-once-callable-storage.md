# ADR-0226: Invocation-Owned Once-Callable Storage

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #303 main `2657176a`, ledger272
- Related contracts: ADR-0031/0036, ADR-0082/0083, ADR-0106/0111,
  ADR-0143 and ADR-0218/0219/0221/0222/0225

## Completed ownership inventory and current requirement

Native expression identity now survives binding through one common owner.
It does not supply a physical owner for a value generated once and consumed
from several independently emitted coroutine regions.

The existing verifier correctly attributes every ordinary callee's transitive
writes to each coroutine caller. Putting a shared value in an ordinary
subroutine therefore does not solve the ownership problem: both callers
still appear to write its registers. Removing that propagation generally
would expose suspended producer state to another caller's writes.

There is a narrower native lifetime. A uniquely gated, non-suspending callable
can execute its value-producing body at most once per statement invocation.
Its exclusive writes belong to that invocation, not to whichever coroutine
first calls it. This decision proves that distinction using existing
instructions, regions and register storage. It adds no cache descriptor,
opcode, VM value container, syntax dependency or generic sharing switch.

The existing uncorrelated query cache is a real migration consumer. Its once
gate is currently emitted around each caller. Native places that gate inside
the reusable subquery body. Moving the existing gate into its owned body
preserves query identity and results while making its lifetime explicit.

| Responsibility | Pinned or current source |
| --- | --- |
| Reuse an existing subquery and put OP_Once inside its body | `sqlite3.c:116711-116760` |
| Mark invocation-once state before executing its body | `sqlite3.c:98945-98988` |
| Allocate actual once destinations and separate reached/deferred generation | `sqlite3.c:118698-118740` |
| Keep callable temporary registers separate from callers | `sqlite3.c:120501-120560` |
| Existing region kinds and typed call/return operations | `SubroutineDescriptor`, ADR-0082/0083 |
| Canonical acyclic call graph and common capture contexts | `AnalyzeSubroutineGraph`, `program.cpp:2398-2731` |
| Definite input/output and invalidation transfer | `program.cpp:4210-4238,4345-4371` |
| Current transitive write propagation and coroutine ownership | `program.cpp:4230-4233,4423-4503` |
| Protected constant construction and reentry rejection | ADR-0143/0221, `entry_initializers`, `entry_reentry` |
| Current gate registration, call and result copy | `EmitSubqueryValue`, `plan_lowering.cpp:6050-6119` |
| Current private body and return labels | `EmitReadSubroutine`, `EmitSubqueryBodies` |
| Canonical runtime once, call and cleanup | `Vm::Impl::Execute`, `ClearExecution`, ADR-0219 |

Fresh main272 research replays the original81 generation and11 signed-alias
cases through native and WriteSession, with80 applicable ReadSession cases.
All previous outcomes remain unchanged and both current read/write SELECT
protocols agree. These are research observations, not a red or a general
generation-parity claim. The retained eager-alias observations still require
shared generated-site ownership.

## Decision

### Recognize a bounded existing encoding, not a new execution mechanism

A provisional invocation-once callable must be an existing value or read
subroutine with all of these properties:

1. Its first instruction is `OnceInstruction`.
2. Its already-seen target is the region's final instruction, a matching
   `ReturnSubroutineInstruction`.
3. That once-site ID occurs in exactly one instruction in the entire program.
4. The region contains no subroutine call, coroutine-control instruction or
   destructive `MoveInstruction`.

The existing descriptor, branch, reachability, recursion, capability and
return checks remain authoritative. There is no client-row suspension inside
a subroutine. Excluding private calls and coroutine control makes this first
proof non-nested; it does not infer a transitive atomic-body property.

The unique entry gate marks its site before the body, while every later call
returns through the existing already-seen edge. Both successors remain
conservative for definite initialization. The gate does not define a SQL
value, grant a capture or make an uninitialized output readable.

Runtime errors and instruction-budget exhaustion remain terminal through
the existing VM cleanup. They do not expose a partly initialized body to
another caller. Reset/rebind and independent VMs use the existing invocation
epoch and register lifecycle; row suspension and coroutine cancellation do
not invent a second once lifetime.

### Prove exclusive invocation-owned writes

Shape recognition alone grants no permission. A provisional body qualifies
only when its may-written registers:

- have no ordinary writer outside that body, including main, another
  callable, another provisional body or a coroutine;
- do not overlap any coroutine output range; and
- do not rely on a reenterable entry-constant construction.

The existing one-shot main constant construction is the sole permitted
outside construction. Parameter, arithmetic and function writes outside the
body are not constructors. Coroutine receiver destinations remain ordinary
writes and cannot overwrite a protected value. Read-only captures still
require the canonical initialized-input and common-ancestor proofs.

Use the existing region IDs, read/write summaries, call order and register
ownership allocation. Do not introduce a second register-reference visitor,
another dataflow lattice, per-expression runtime cache or a dense
scope-by-register mirror.

During ordinary dataflow, retain all input, definite-output, invalidation,
read and capability transfers. Provisionally omit only a recognized body's
write-effect propagation at its call sites. Its body has no nested private
calls, so its own write summary is direct. After analysis, use those existing
summaries and coroutine output ranges to prove exclusive ownership.

If the proof fails, restore that body's ordinary transitive effects through
the existing acyclic call order before running the normal ownership checks.
Do not turn failed recognition into a new rejection of an otherwise valid
legacy program. In particular, a main-only gated body with an outside writer
or reenterable construction must retain its prior ordinary interpretation.

For a successful proof, identify the body as the logical register owner
instead of attributing its writes to every coroutine caller. Outside writes
remain invalid. Outside reads gain no initialization fact: they still need
the existing explicit construction or definite-state evidence. Coroutine
output/move, private-capture, sibling and constructor-reentry restrictions
remain in force for all other state.

Ordinary subroutines, nonunique once sites, wrong gate/return shapes, bodies
with private calls or coroutine control, and destructive moves keep their
current interpretation. This is not permission to share arbitrary callable
scratch or mutable resources.

### Preserve bounded analysis and capability ownership

Perform proof discovery only when coroutines and relevant once-gated shapes
exist. Temporary metadata is bounded by the actual subroutine/once-site
populations, not configured maxima. Reuse the current register-owner table
for provisional conflicts and final logical ownership.

Count global once-site uses through bounded bitsets or equivalent existing
checked storage. Reuse the canonical effect summaries and call order when
restoring failed candidates. Charge successful and unsuccessful discovery,
site scans, conflict checks and effect restoration to the existing analysis
memory/work budgets. Overflow or exhaustion returns the existing typed error;
it never silently enables the proof.

Do not alter `InstructionOwnership`, capability reference checking, read-body
closed-return requirements, reusable-membership cleanup, coroutine ancestry
or cancellation metadata. A private read capability retains its physical
owner and existing lifecycle. This register proof is not a resource-sharing
exception and does not change retained program or VM state.

Programs without applicable shapes keep their existing storage path. Public
instruction, descriptor, program-input and SQL-value layouts do not change.
Measure actual private analysis work/allocation deltas; preserve all limits.

### Move the current query gate to its callable owner

Keep `StatementLoweringState::subqueries`, the existing result register,
optional once-site ID, lazy subroutine registration, captures and labels.
Emit the existing once gate at `EmitReadSubroutine`'s entry, targeting that
same body's existing final-return label.

Callers emit the same typed call and cached-result copy, without a second
caller gate or per-call cached label. Keep the existing main result-NULL
construction and every normal body result assignment. Captured queries keep
their existing inline path and receive no once site.

Do not require every read body to qualify for the new storage proof. A body
containing nested private work retains ordinary ownership, while its query
gate still belongs to that definition. Preserve query identity, source
scheduling, empty scalar/EXISTS behavior and shared read/write compilation.

Record call/return/instruction and allocation changes. A repeated cached call
now reaches its internal gate rather than skipping the call externally, as
native does. This is a mechanism-aligned migration, not a speed claim.

## Focused proof and ordered delivery

Use two related, buildable sources under ADR-0106:

1. Add the narrow verifier ownership proof, test-first, with existing VM
   execution and lifetime evidence.
2. Migrate the current uncorrelated-query gate from a qualified green
   baseline, preserving all non-target SQL and diagnostic observations.

The first owning red is a uniquely gated shared callable with an explicitly
constructed result, used by two otherwise disjoint coroutines. It must fail
for the current ownership conflict, not an unrelated malformed control edge
or uninitialized value.

Cover main and multiple-coroutine callers, repeated activation/cancellation,
TEXT/BLOB copies through moved producer outputs, reset/rebind, independent
VMs, scalar errors and instruction-budget failure. Keep the result home
separate from coroutine output/receiver ranges.

Retain rejection of nonunique sites, wrong entry/return targets, uninitialized
results, outside writes, conflicting logical writers, private-call/control
and move bodies, captured private inputs without a common ancestor, output
overlap and protected-construction reentry. Include legacy-positive controls
where failed recognition must fall back rather than impose a new restriction.

Exercise exact analysis-memory/work boundaries and the owning complete
allocation-failure sweeps without raising their ceilings. The migration
retains scalar/EXISTS, empty, captured, ordered, mutation-host, error and
allocation controls, plus the92-case current generation/signed baseline and
existing callback/alias observations.

Measurement probes use unchanged source and matched flags, but are compiled
against the corresponding library's consumed headers. Do not reuse a C++
object across changed public class definitions merely because ABI/layout
appears unchanged. Preserve genuine failures and distinguish current execution
from reconciled predecessor evidence.

Independent design acceptance precedes production changes. Each source needs
focused evidence, complete fast units, implementation review and a normal
commit/ledger entry. Final selected local/hosted/integrated/main gates apply
to the exact related batch head.

This narrowly supersedes unconditional coroutine attribution of writes for
the proven encoding only. It does not activate ordinary constant-function
factoring, shared eager constructor generation, deferred expression queues,
signed-alias generation, diagnostic arbitration, IN or a later DAG node.
Those remaining mechanisms still need their own complete accepted design.
