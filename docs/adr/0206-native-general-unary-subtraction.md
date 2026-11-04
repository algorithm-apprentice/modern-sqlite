# ADR-0206: Native General Unary Subtraction

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #292 main `1b65fab9`, tree `4a453b37`, ledger214
- Related contracts: ADR-0036, ADR-0106/0111, ADR-0160/0164 and ADR-0190/0200/0204

## Completed mapping and bounded requirement

PR #292 and its branch retirement are complete. A freshly rebuilt actual-main
trace preserves all65 retained preparation/value observations, including24
differing observations, not24 independent defects. General name/value
generation, scalar-width ordering, constant factoring and IN remain open.

The existing unary runtime repair can be separated from the still-unaccepted
diagnostic owner. Native expression generation distinguishes a direct
numeric literal from every other minus operand. Modern currently emits the
same typed negation instruction for both.

| Responsibility | Pinned source or current owner |
|---|---|
| Replace an existing leading UPLUS when constructing PLUS/MINUS | Pinned amalgamation `sqlite3.c:185330-185344`, generated `parse.y` rule216 |
| Alias replacement copies the original expression root | `resolve.c:68-106` |
| Direct numeric sign generation | `expr.c:4408-4463,5372-5386` |
| General minus generates integer zero minus its operand | `expr.c:5387-5395` |
| Arithmetic NULL, checked integer, REAL and NaN rules | `vdbe.c:1951-2038`; ADR-0200 |
| Existing parenthesis/positive syntax peek | `src/syntax/literal_expression.hpp::UnaryNumericLiteral` |
| Existing alias-only bound peek | `src/binder/native_truth.hpp::LogicalOperandBinary` |
| Existing expression homes and shared constant owner | `PlanLowerer::Home`, `StatementLoweringState::zero_constant`, `EnsureZeroConstant` |
| Existing iterative value/branch emission | `PlanLowerer::EmitOperationLoop`, `BeginValueEmission`, `ResumeEmission` |
| Canonical subtraction implementation | `SqlValueAccess::Subtract`, already used by VM binary subtraction and selected defaults |

Twenty-three additional matched caller observations on main214 retain exact
REAL bits, callback argument storage classes and engine-specific instruction
counts. The callback deterministically returns its argument; neither engine
calls it during preparation. Eleven zero-sign differences cover text/blob,
columns, functions, COLLATE/likelihood, scalar queries, nested negatives and
alias construction. A twelfth difference is the separately unclosed signed
integer alias boundary. The fixture is owned, authenticated and removed.

The alias controls distinguish syntactic `-(+x)` from negating an alias whose
definition is `+0.0`. Native construction removes the former positive wrapper
before resolving `x`; it does not remove a positive wrapper copied later by
alias replacement. Removing all bound positive wrappers would be incorrect.

The native and Modern values for direct `-0.0` and its parenthesis/leading-plus
forms already match as negative zero. General `-'0.0'`, `-column` and
`-(-(-0.0))` instead require positive zero. Changing the VM primitive globally
would break the direct-literal controls.

## Decision

### Normalize the actual unary constructor, not resolved aliases

Extract the existing parenthesis/leading-positive loop from
`UnaryNumericLiteral` into one borrowed syntax-operand helper. The numeric
peek calls that same helper and retains its exact result/validation behavior.

The binder's existing unary continuation uses this operand only for original
PLUS/MINUS syntax. Preserve one actual outer unary node, its source span,
construction flags, ON origin and scalar/name scope. Do not remove negatives,
COLLATE, likelihood or other operators. NOT and BITNOT retain their current
operands.

Do this before binding the selected child. Never perform the same rewrite
through a resolved alias. Keep parser AST ownership, normalization events,
height limits and the existing continuation container. This adds neither a
second syntax walk nor a reconstructed expression.

### Emit general minus through existing subtraction

Extract the alias-only peek already used by `LogicalOperandBinary` into one
private borrowed bound-expression helper in the same header. Both logical
selection and unary classification use it; neither skips COLLATE, likelihood
or a positive unary node introduced through alias replacement.

Preserve the current direct numeric-literal path. Its admitted bound
numeric literal values and INTEGER/REAL token descriptors keep their existing
typed negation emission. This source does not introduce new integer range
errors or claim that a materialized value repairs native integer-token
generation. The signed integer alias boundary remains explicitly unclosed.
Boolean, text/blob, NULL, column/rowid, parameter, function, query and composite
operands use the general arithmetic path.

For general minus, use the existing shared integer-zero constant and existing
expression-home registers. The unary expression's home and its child's home
are distinct bound IDs; no new register allocation, scratch owner, public
carrier or bytecode instruction is needed. Emit zero into the unary home,
evaluate the operand once into its own home, and append the existing binary
subtraction into the requested destination. The existing arithmetic kernel
already permits that destination to alias either input.

Use one additional phase of `EmitOperationLoop`, not another recursive
emitter or evaluator. Preserve failure propagation from constant creation,
instruction append and child generation. Keep the existing per-statement
zero owner, query frames, branch paths and shared read/write lowering.

Do not change `UnaryOperation::kNegate` or its VM implementation. Its existing
direct numeric behavior remains available; this correction selects the
proper existing bytecode operation for general SQL minus. Binary subtraction
continues to preserve NULL, arithmetic text/blob typing, checked overflow,
REAL/NaN publication and reusable register storage.

### Preserve unclosed generation boundaries

This source does not add a deferred initializer queue, reached-once cache,
constant deduplication, alternate optimizer, eager all-leaf materializer,
bind-retry path, new public representation or verifier exception.

Do not activate ordinary negative hexadecimal diagnostics while complete
name/value generation remains unresolved. Preserve the existing winners in
all retained ordering, eager VALUES and construction-barrier controls.
Selected INSERT defaults and catalog extraction continue using their already
accepted separate owners.

The new zero load is ordinary existing emission, not a claim that native
CodeTemp factoring is implemented. Record actual instruction/allocation work;
do not infer opcode parity or a speed improvement. Complete the general
factoring/initialization owner separately before claiming that prerequisite
closed.

## Test-first and delivery

The actual native format target passed its registered inputs, but command
authentication found both modified private headers absent from that list.
Register `src/binder/native_truth.hpp` and
`src/syntax/literal_expression.hpp` in the canonical CMake format inputs.
Preserve the earlier target success only for its actual registered inputs;
it did not complete coverage of this change.

The root CMake change selects the unchanged verification policy's full
profile. Retain every resulting compiler/runtime, native, quality and four
bounded fuzz requirement. Reconfigure and reconcile unchanged source,
compile commands, caches, images, test definitions, environment and process
mode before reusing successful prefix evidence. Preserve its actual execution
provenance; differing or incomplete evidence requires fresh qualification.
Do not substitute an ad-hoc per-file check or downgrade this escalation.
Final complete qualification and exact-tree review still precede source
acceptance and commit.

Start with one lowering/runtime regression for positive REAL zero from
`-'0.0'`; record its genuine failure before implementation. Cover direct and
wrapped numeric negative zero, repeated negatives, text/blob/parameter/
column/function/query operands, NULL and integer-overflow promotion.

Use the existing lowering environment and deterministic callback helpers to
observe alias arguments, including an unused EXISTS projection, without
adding a session registration API. Preserve the original callback result and
check preparation does not invoke it. Exercise read and write consumers of
the shared emitter and retain borrowed/moved owners.

Add a bounded case to the existing complete lowering allocation-prefix
sweep for the newly selected zero/subtraction path. Retain all original
depth, default-stack, allocation, instruction and timeout bounds. Existing
invalid-AST, alias, representative/grouped and integer-token controls remain
part of the owning selection.

Reconcile the current65 and the new23 paired observations: no formerly
matching case may regress. Preserve the known integer diagnostic/alias and
generation-order differences rather than labeling either whole population
compatible. No production instrumentation or additional timing campaign is
required for this correctness correction.

After accepted design, focused red/green, complete fast units, the actual
change-aware local plan and independent implementation review, commit one
coherent source and append its ledger record. Related generation corrections
may follow in a normal ADR-0106 batch only after that source is accepted.
Final-head local/hosted/integrated gates and qualified-main retirement remain
required before delivery. General generation, factoring, IN and node43 stay
open.
