# ADR-0082: Verified Static Value Subroutines

- Status: Accepted
- Date: 2026-10-14

## Context

ADR-0053 requires verified subroutines and coroutines before subqueries, CTEs,
views, and triggers. The existing bytecode verifier proves register
initialization, exact capability states, reachability, branch targets, and
bounded analysis work. Ordinary jumps cannot encode a reusable return path.

Pinned `src/vdbe.c:1170-1296` implements Gosub/Return and coroutine suspension
by storing instruction addresses in integer registers. Modern preserves
shared SQL-value registers and explicit continuation behavior, not untyped
program counters masquerading as SQL values.

This control-flow prerequisite is independent of the remaining stored and
automatic join-index work. It may proceed without declaring those scopes or
the advanced-SQL node complete. Resource-aware subroutines and coroutines
follow this bounded value/control foundation before SQL grammar is broadened.

## Decision

### 1. Declare immutable, typed callable regions

Add a strong `SubroutineId`, immutable half-open instruction regions, and
typed call/return instructions. A descriptor owns no AST, plan, callback,
register pointer, or mutable program.

Regions are nonempty, disjoint, in bounds, and exclude the main entry.
Branches and fallthrough remain inside their lexical owner. Returns identify
their owning subroutine. Main cannot return, and a subroutine cannot halt or
publish a client result row.

The first callable surface contains value/register operations, comparisons,
scalar calls, ordinary internal branches, and nested static calls. Cursor,
temporary-storage, aggregate, rowid-list, mutation, and schema capabilities
remain main-owned and cannot be referenced inside a value subroutine. A later
resource-aware contract must prove those effects rather than weakening the
current capability lattice.

Builder-owned entry and last-instruction labels resolve before publication
to the descriptor's half-open range. This reuses ordinary label ownership
without admitting a one-past-end branch label. Existing foreign/unbound label,
range, ownership, and owned-byte checks remain authoritative.

### 2. Reuse one definite-initialization analysis

Build and validate an acyclic static call graph without recursive C++ traversal.
Reject self/mutual recursion and uncalled regions. Analyze callees before
callers with the existing instruction transfer and register-bitset lattice.
Do not implement a second approximate bytecode checker.

For each subroutine infer:

- required input registers read before a definite local write; and
- registers definitely written on every reachable normal return.

At a call, require its inferred inputs and union its definite writes into
the caller's state. Preserve every other initialized caller register.
Never merge unrelated callers into one shared return-state intersection.
This supports multiple call sites with distinct caller-private registers.

Every region and main retain the existing unreachable-code rejection.
All normal return paths contribute to the output intersection; loops remain
subject to the VM instruction budget. Record the static maximum call depth.

Region ownership, graph traversal, input/output summaries, and dataflow states
are charged to the existing checked analysis-memory and edge-work budgets.
Programs without subroutines retain their current analysis and allocation
path; no empty-feature side allocation is introduced.

### 3. Execute through a bounded typed continuation stack

The VM retains the program's shared SQL-value registers. A call pushes only
the typed callee identity and return instruction address, then enters the
declared region. A return consumes that exact continuation.

Reserve continuation capacity from the verified static maximum depth during
VM creation. Calls and returns perform no stack allocation, value cloning,
runtime compilation, or plan interpretation. Addresses never occupy SQL
registers.

Instruction limits include calls and returns. Error, exceptional failure,
reset, and destruction release continuation state through the existing
execution lifecycle; reset retains parameter bindings and starts again at
main. Caller-owned capabilities remain unchanged across a value call.

### 4. Deliver the control prerequisite before consumers

Begin with a focused multiple-caller program specification and observe its
failure before implementing the new contract. Cover shared outputs,
caller-private registers, nested/repeated calls, conditional writes, loops,
all malformed region/ID/branch/return combinations, recursive and unreachable
graphs, budget boundaries, typed runtime errors, reset/rebind, and allocation
failures.

Compare hand-authored arithmetic/control results with independent native
scalar results where applicable. Add module-owned generated/model evidence;
no SQL parser acceptance or persistent format changes are part of this
package. Review design, core, and incremental evidence separately, then merge
one coherent slice only after all required gates pass.

### Closure evidence

The focused multiple-caller specification first failed because the typed
subroutine contract was unmodeled. It then passed through the shared verifier
without merging caller-private registers. The final fast tier passes 1,225
unit cases; complete Release and ASan/UBSan tiers pass 1,271 each, and the
independent GCC tier passes 1,269. All 16 focused control/allocation cases also
pass on Debug, Release, ASan/UBSan, and GCC after the derived-count-only lint
correction.

Executable one-, eight-, and 64-level chains establish the exact maximum
depth, instruction counts, return continuations, and reset/rebinding behavior.
A separate arithmetic contribution model checks 32 reproducible shared-call
DAGs under three bindings, including a previously visited leaf before a deeper
caller. One thousand sanitized repetitions complete 96,000 modeled execution
lifetimes. A looping body uses the same definite-register lattice as main.

Malformed IDs, empty/overlapping/out-of-range regions, cross-region branches,
foreign labels, mismatched/main returns, self/mutual recursion, uncalled code,
forbidden capabilities/results/halt, missing inputs, conditional outputs,
and exact memory/work-budget boundaries are rejected.

Program creation and VM creation cover every measured allocation failure.
A failed builder declaration preserves its ID and an exact owned-byte budget.
Eight thousand repeated calls and returns allocate nothing after preparation.
Typed scalar errors, instruction-budget interruption, allocation failure,
reset, and recovered execution retain the canonical VM lifecycle.

All 257 existing native comparisons, six unsupported boundaries, and 42
archived runner/work contracts remain unchanged. Existing statements retain
their allocation budgets. No SQL syntax, runtime compiler, persistent format,
private resource store, or capability-state relaxation is introduced.
Direct design, core, and incremental evidence review has no unresolved issue
in this value/control scope. Changed-translation-unit clang-tidy, formatting,
engineering discipline, and dependency-graph validation pass.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 subquery-control prerequisite.

Contracts: ADR-0031, ADR-0032, ADR-0048, ADR-0053, ADR-0056, ADR-0058,
ADR-0062, ADR-0076, and ADR-0081.
