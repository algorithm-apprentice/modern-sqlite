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

## Traceability

Node: `implement-advanced-sql`, ADR-0053 subquery-control prerequisite.

Contracts: ADR-0031, ADR-0032, ADR-0048, ADR-0053, ADR-0056, ADR-0058,
ADR-0062, ADR-0076, and ADR-0081.
