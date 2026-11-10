# ADR-0244: Generated Integer Expression Identity

- Status: Accepted
- Date: 2026-11-10
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #317 main `340d72e63`, ledger334
- Related contracts: ADR-0106/0111/0206/0207/0224/0225/0241/0242

## Requirement and completed bounded inventory

Native general unary minus requests a generated integer zero through
`sqlite3ExprCodeTemp` before requesting its operand. Reusable initialization
can therefore compare that generated node with an earlier ordinary
expression, not just compare two expressions from the SQL syntax tree.

The shared published comparison currently requires two bound expression IDs.
An invented ID, a second bound arena, or a separately implemented zero
classifier would not preserve that contract. The semantic
`BoundExpressionIntegerLiteralValue` helper is also not a replacement: it
traverses unary signs, whereas native generated-integer identity does not
equate an integer node with every expression yielding the same number.

This is the bounded identity prerequisite for the remaining native
temporary/fixed-factorable generator. It does not activate that generator or
claim a new SQL result, callback-count, or performance correction.

| Responsibility | Canonical source |
| --- | --- |
| Generated inline integer zero and its temporary request | `sqlite3.c:118169-118192` |
| Reusable comparison before once/deferred classification | `sqlite3.c:118698-118740` |
| Root wrapper handling belongs to CodeTemp, not comparison | `sqlite3.c:118777-118799` |
| Exact inline-integer identity before ordinary node comparison | `sqlite3.c:119465-119485` |
| Canonical literal identity, alias traversal and retained origins | `NativeExpressionComparison::NativeLiteralIdentityOf` |
| One native literal comparison, including inline32 and sign shape | `NativeExpressionComparison::NativeLiteralsEquivalent` |
| Stable common read/write syntax and expression ownership | `BoundSelect::Impl`, ADR-0225 |
| Published owner, ID and origin-arena checks | `NativeBoundExpressionsEquivalent` |
| Different semantic integer-value query | `BoundExpressionIntegerLiteralValue` |
| Existing moved/read/write/invalid-input and allocation controls | `bound_select_test.cpp`, `bound_select_oom_test.cpp` |

The pinned amalgamation remains SHA256
`4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6`.
Its generated node has `TK_INTEGER`, `EP_IntValue`, and a signed32 value.
The native comparison admits equal inline values; a token-based large
integer, REAL, Boolean keyword, string, parameter, or unary expression is
not interchangeable merely because evaluating it could produce that value.

## Decision

### Extend the existing comparison owner

Add `NativeBoundExpressionMatchesInteger(const BoundSelect&,
BoundExpressionId, std::int32_t)`, returning the existing
`std::expected<bool, BindError>`. The integer parameter denotes a generated
native inline-integer node, not an evaluated SQL value or an expression ID.

Implement it inside the existing private `NativeExpressionComparison`.
Obtain the bound operand's identity with `NativeLiteralIdentityOf`, construct
one transient `NativeLiteralIdentity` for the supplied inline integer, and
use `NativeLiteralsEquivalent`. A valid nonliteral operand returns false.
Do not add another numeric parser, syntax walker, comparison algorithm,
owning value, origin table, or runtime register representation.

Preserve the canonical handling of aliases and existing parser-generated
literal nodes. Match admitted small-integer spellings through the existing
inline classifier, including its current separator and hexadecimal rules.
Do not unwrap semantic unary-plus/minus, COLLATE, likelihood, casts or
functions, and do not compare a parameter's eventual binding.
CodeTemp's separate wrapper normalization remains a future caller duty.

Keep the native literal kind and signed-minimum distinction. In particular,
integer zero does not match REAL zero, Boolean false, text zero, a semantic
positive or negative wrapper, or an unrelated folded signed minimum.
An ordinary differing integer is simply a valid nonmatch.

### Share the published validation boundary

Use the same common owner and existing typed errors as the two-expression
operation. A moved-from owner or out-of-range ID is invalid input, not a
nonmatch. An inconsistent origin arena remains an explicit internal error.

Extract the existing published ID/arena validation into one private helper
on the common implementation and use it from both published entry points.
Retain operand-range checks before the origin-arena check and preserve the
existing two-operand diagnostic details. Do not add validation work to the
binder's already-valid internal comparison hot paths.

The operation borrows the existing stable syntax and origins only during
the call. It must remain valid after caller SQL retirement and common-owner
moves, including the shared mutation owner. It neither mutates that owner
nor retains a new reference after returning.

Valid identity queries create no owning allocation or new traversal stack.
Retain the existing exception/typed-error boundary for diagnostic creation;
do not add a catch, a materialized fallback, or a new error family.

## Focused proof and bounded delivery

Obtain independent design acceptance before production changes. Add one
focused public-interface test and record the missing-operation build failure.
Then implement the smallest shared comparison extension.

Cover actual parsed integer spellings and noncoercive zero distinctions,
signed32 boundaries, existing generated literal forms, parameter/nonliteral
controls, aliases, retired short source storage, moved read/mutation owners,
and invalid IDs/moved-from owners. Prefer a few owning tests with meaningful
case tables over tests of accessors or constants.

Retain every existing native-comparison and binder allocation-prefix control.
Exercise the valid new comparison under the existing owning allocation
observer to verify it adds no allocation or retained state. Run focused
green, complete fast units, the actual strict changed-test compiler/checker
owners, and the change-selected closing plan. Preserve all original
deadlines, workloads, allocation limits, and hosted requirements.

The public header change retains the full relevant compiler matrix; a small
implementation does not exempt it from that plan. Independent source review
and normal node/ADR/ledger traceability precede any dependent source.
Related slice batching still requires an accepted design for each successor;
this document does not approve unspecified generator changes.

The existing initializer ownership composition probe is only evidence that
ordinary main-owned initialization can coexist with protected constant
construction. It is not runtime or general-generator acceptance. Ordinary
caller modes, fixed argument lifetime, FIFO initialization, original
producer generation, and preparation-error arbitration remain separate
integrations. The nine deferred-callback and two unrequested-constructor
differences remain open. No IN, node43, or later-node completion is claimed.
