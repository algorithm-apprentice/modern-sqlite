# ADR-0215: Shared Signed Literal Materialization

- Status: Accepted
- Date: 2026-11-05
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #294 main `5f926bfd`, ledger231
- Related contracts: ADR-0015, ADR-0106/0111, ADR-0160 and ADR-0200/0206/0207

## Completed primitive mapping and current requirement

The remaining generation owner needs original numeric syntax, not arithmetic
negation of an already materialized positive value. Existing signed-alias
research records native INTEGER INT64_MIN for a decimal INTEGER token whose
positive value is REAL. The hexadecimal minimum has a different signed
generation error. Those observations retain their original identities; they
are not new main231 execution or a source red.

The relevant pinned primitive is already mapped and implemented:

| Responsibility | Existing source |
| --- | --- |
| Apply a sign to parsed REAL, including zero and subnormal bits | Pinned amalgamation `sqlite3.c:117205-117214` |
| Signed decimal minimum, hexadecimal rejection and REAL fallback | Pinned amalgamation `sqlite3.c:117223-117256` |
| Direct numeric sign versus general zero-subtraction | Pinned amalgamation `sqlite3.c:118168-118192` |
| Canonical signed conversion and underscore normalization | `src/binder/bound_select.cpp::MaterializeLiteralValue`, `ParseIntegerLiteral` |
| Checked public bound-token bridge | `MaterializeBoundLiteral` |
| Existing signed consumers | `MaterializeSyntaxConstant`, `BindUnaryLiteral` |
| Original numeric spelling and source ownership | ADR-0207's token kind/span and immutable bound owner |
| Constant publication and typed materialization errors | `StatementLoweringState::literal_constants`, `PlanLoweringErrorCode::LiteralMaterialization` |

ADR-0200 already parameterized the private conversion kernel with a separate
negative flag. Selected literal defaults and the ordinary, already-valid
decimal-minimum shortcut use it. The public bound-token bridge currently
exposes only positive generation, and these signed callers bypass that
descriptor entry point.

This source makes that existing signed primitive available through the same
typed bridge and uses it at the two existing signed call sites. It adds no
parser, expression carrier, cache, evaluator, bytecode operation or generation
scheduler. The complete ordinary name/value/width/factoring owner remains a
separate, unclosed requirement.

## Decision

Add a `negative` argument, defaulting to false, to the existing declaration
and definition of `MaterializeBoundLiteral`. Forward it to the existing
private conversion kernel after the bridge's existing kind/token validation.
Retain one function, not a parallel signed materializer or compatibility shim.
Existing two-argument source calls keep positive behavior.

Negative generation accepts only INTEGER and REAL descriptors. Preserve
lexical rejection before signed-minimum recognition. Signed nonnumeric or
malformed descriptors return the existing structural invariant error, never a
fabricated value. Valid out-of-range hexadecimal tokens use the existing typed
value error with the supplied source span and normalized signed spelling.
Keep positive hexadecimal interpretation, decimal REAL fallback and exact
REAL sign bits unchanged.

At the existing selected-default direct-numeric path and ordinary decimal-
minimum shortcut, construct a transient `BoundLiteralTokenExpression` over
the same source bytes and call the bridge with the negative flag. Keep the
callers' current recognition and validation order, including their established
syntax diagnostics. The descriptor boundary intentionally repeats token
validation through the existing checker. This adds no AST traversal or new
validation/allocation mechanism; it is not a speed improvement.
The transient view does not outlive the call or enter a program/VM.

Do not change selected-column ordering, final affinity, the general default
negation continuation, CREATE admission or missing-record extraction. Do not
change the positive ordinary binder path or activate ordinary negative
hexadecimal diagnostics. Lowering's existing two-argument call, numeric-origin
classification, VM negation and general zero-subtraction remain unchanged.
No bound value, expression, error or program carrier grows.

## Test-first and delivery

Add one focused owning test for signed materialization through the public
bridge before its signature changes. Retain rejection of the unsupported sign
argument as the interface red, then cover INTEGER minimum/nearby REAL fallback,
hexadecimal boundaries, underscores, negative zero/subnormal bits and typed
signed overflow. Exercise malformed numeric descriptors and unsupported
signed nonnumeric descriptors without suppressing structural/value distinctions.

Use the existing positive materializer, selected-default, source-lifetime,
signed-minimum, deep-continuation and allocation-prefix controls to preserve
both current consumers. No SQL alias fix, new error-order compatibility,
callback-count correction or performance improvement is claimed.

Run focused green, complete fast units and the actual selected closing plan.
Keep original bounds, case populations and failure evidence. Independent
design and source review precede a normal coherent source232 commit and
append-only ledger entry. Any following generation-owner integration needs
its own accepted design and behavioral red; this primitive does not grant
that broader admission. Final exact-head/integrated/main gates still apply.
All remaining INTEGER/name/value/width/deferred/factoring findings, IN and
node43 remain open until their own mechanisms are completed.
