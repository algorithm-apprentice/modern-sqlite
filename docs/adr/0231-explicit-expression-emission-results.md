# ADR-0231: Explicit Expression Emission Results

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #306 main `eb0b2c84`, ledger286
- Related contracts: ADR-0036/0106/0111/0216/0218/0226/0229/0230

## Current requirement and completed bounded inventory

The remaining native generation owner must distinguish a caller's requested
destination from the register actually returned by a generated operand.
SQLite's temporary-expression interface may return a retained register instead
of its requested scratch. A global bound-expression home is neither that
result contract nor a generation-site identity.

The current continuation emitter carries a requested output into each child,
but completion carries no value. Arithmetic, truth and comparison parents
look up the child's `Home` again after generation. That is consistent with
the current fixed-output emitters, but it does not explicitly transport the
result that the parent consumes.

Fresh main286 fixed-work execution of the unchanged eight-case registered
function probe retains five callback-count differences: native/Modern1/3 for
projection, conditional and correlated queries;2/4 for an alias guard plus
projection; and2/6 for two source occurrences. Empty, LIMIT0 and uncorrelated
query controls match. All rows match and neither engine invokes the callback
during preparation. The probe was rebuilt with the current canonical native
compiler flags and consumed definitions. These are compiler/VM observations,
not session-facade evidence, a TDD red, or a timing claim.

This decision makes the current compiler result contract explicit. It does
not activate the missing generation behavior or claim to close those gaps.

| Responsibility | Pinned or current source |
| --- | --- |
| Return an actual temporary or retained operand register | `sqlite3.c:118776-118799` |
| Force a caller destination when required | `sqlite3.c:118807-118849` |
| Separate requested and reusable once destinations | `sqlite3.c:118698-118740` |
| Retain operand registers through reordered/null-skipped evaluation | `sqlite3.c:115232-115278` |
| Separate callable temporary ownership | `sqlite3.c:120501-120560`; ADR-0218 |
| One iterative value/branch/membership emitter | `EmissionRequest`, `EmissionFrame`, `EmitOperationLoop` |
| Existing requested outputs and empty completion | `DescendValue`, `DescendHome`, `EmissionComplete` |
| Parents that reconstruct child homes | `EmitBinaryResult`, `EmitComparisonResult`, `FinishTruthTest`, `FinishQueryLogicalEmission` |
| Existing typed register allocation and ownership | `ExpressionRegister`, `Home`, `ArgumentBlock`, `StatementLoweringState` |
| Existing continuation/error boundary | `compiler_detail::ContinuationStack`, `LoweringResult`, `InternalFailure` |
| Actual copy ownership/cost | `Vm::CopyRegister`, `SqlValueAccess::Copy`, ADR-0226 |

`CopyInstruction` preserves an existing explicit stable borrow, but copying an
owned TEXT/BLOB can move bytes and grow retained destination storage. Do not
hide a future returned-register mismatch behind unconditional extra copies,
or change VM borrowing merely to keep a void compiler interface.

## Decision

### Return one typed completion through the existing continuation

Extend the existing completion value to distinguish a completed value
register from completed branch/membership control. Use the existing
`RegisterId` and a small optional result in compiler-local state; do not add
a public program carrier, runtime value container, instruction, descriptor,
second emitter or per-expression result table.

Every current value emitter reports the register its actual instruction
defines. Every control-only completion reports no value. The operation loop
checks that the completion kind matches the child request before resuming
its parent. A mismatch returns the existing typed internal-invariant error;
it never invents register0 or silently treats absent output as success.

Keep the public/internal service-facing fixed-destination signatures of
`EmitExpression` and conditional-branch entry points. Their wrappers validate
the returned completion. This source does not introduce temporary/factorable
caller modes or permission for an existing fixed-output request to return a
different destination.

### Consume the emitted child result, not a later home lookup

Pass the completed child value into the canonical resume operation. Retain
an earlier operand's register in the existing phase-owned frame state while
the next child is generated.

Arithmetic, general unary, truth, comparison and query-aware logical/null
paths consume those concrete registers. Change their existing finishing
helpers to receive operand registers while retaining the same operation,
affinity, collation, labels, syntax-order correspondence and output.
Do not introduce a second arithmetic/comparison helper.

Preserve left/right ordering, the reordered subquery operand, the first
operand used on a short-circuit edge, and every NULL branch. Control children
do not become values. Fixed conditional arms, scalar argument blocks and
membership cells keep their existing destinations and mutation rules.
Preserve projection/aggregate representative handling and lexical captures.

Keep one iterative `ContinuationStack`. Reuse the current frame's per-phase
register fields where their ownership is clear; do not add an arena-wide
binding mirror, map or configured-maximum reservation. Do not switch a
physical instruction owner while merely forwarding a child result.

### Preserve current generation, resources and errors

This is a behavior-preserving refactor from a qualified green baseline.
Every current emitted instruction, operand register, label, resource count
and evaluation order must remain unchanged. Do not add a copy, once site,
initializer, register allocation or generation prepass.

`Home` and `ArgumentBlock` remain the canonical allocation/access boundary
when a request chooses its destination. Private definitions remain contiguous,
with the same typed subroutine/coroutine owner and cleanup. The builder owns
all register counts; the verifier retains every initialization, ownership,
capability and budget check. No bytecode or VM change is needed.

Keep current typed failures, normal allocation unwinding, source/name/value/
width/collation decisions and all deadlines. Do not alter native identity,
retained constructor roots, registered-name snapshots, callback counts,
reached-once/deferred scheduling or shared eager execution in this source.

## Baseline, proof and bounded delivery

Independent design review precedes production changes. Start from the
existing focused lowerer controls and complete fast-unit green baseline;
do not invent an artificial failing behavior test for this refactor.

Use the existing normal/noncommutative arithmetic, comparison, truth, NULL,
short-circuit, scalar/conditional, linear/reusable membership, mixed-depth,
private-query/coroutine, mutation and ownership tests. Preserve complete
lowerer allocation-failure coverage and limits. Add a regression only if the
refactor exposes an actual missing behavior, not to assert an internal
optional type or accessor.

Reuse the existing matched lowering-allocation/fixed-work probe and the
program-shape observer for allocation calls/bytes, emitted/executed instruction
counts, instruction kinds and register/once/callable counts. Keep the owning
lowerer allocation-failure and retirement controls; binder-only allocation
samples do not substitute for lowering evidence. Reconcile only genuinely
identical baseline inputs, and rebuild probes when consumed definitions or
library versions change. Inspect operand wiring through the owning tests;
equal opcode counts alone are insufficient. The eight-case callback controls
and the retained115 protocol cases remain unchanged, including their
explicitly unclosed native differences.

Run focused green, the complete fast-unit tier, current strict compiler and
checker owners, and the actual change-aware closing plan. Preserve every
selected isolated/native/sanitizer/fuzz/hosted obligation. Record independent
implementation review and normal node/ADR/source traceability before a
dependent slice.

The broader generator still requires complete caller-mode, original-site,
deferred-drain and preparation-error ownership design. This result-flow
contract alone grants no factoring, cache, shared mutable-register, signed
generation, IN or later-node admission.
