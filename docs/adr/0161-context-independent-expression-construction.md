# ADR-0161: Context-Independent Native Expression Construction

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: bounded construction prerequisite before required-unary/IN admission

## Context and completed source mapping

Start from PR #265's verified squash `6f5bb4c`, tree
`ac48e9079e32562ad42310a7ac0c0025f8c6f8f4`. Actual resulting-main CI and
full-library quality passed. The native Windows model passed its original
20-second gate; the earlier undiagnosed hosted stall is retained as evidence,
not relabeled as a diagnosed engine or host cause.

Research for the remaining required negated-hexadecimal boundary confirmed a
smaller prerequisite in implemented behavior. Native
`SELECT missing_name AND 0` returns integer0, while Modern reports a name
error. Adding an unrelated `1 IN (1,2)` result makes Modern return the native
rows. An oversized hexadecimal literal followed by `IS NULL` has the same
context dependence. This is executed source-to-source evidence, not a proposed
new SQL surface.

The pinned SQLite3.54.0 mechanism is:

| Responsibility | Native source |
| --- | --- |
| INTEGER constructor identity and truth flags | `expr.c:994-1006`; ADR-0152's `parse.y`/`util.c` map |
| AND grammar and constructor-time deletion | `parse.y:1354`; `expr.c:1150-1177` |
| Literal IS NULL/IS NOT NULL construction and unary transparency | `parse.y:1392-1436` |
| Constructor flags and local function propagation | `expr.c:1015-1074`; ADR-0152 |
| Later, distinct logical value simplification | `expr.c:2391-2402,4973-5040,6237-6242,6434-6439` |
| Required integer sign handling after name resolution | `expr.c:4426-4458,5372-5395`; `select.c:7780-7783` |
| Early VALUES-owned preparation before outer deletion | ADR-0159's `insert.c`/`expr.c` map |

`sqlite3ExprAnd` discards both operands only when the constructor-known false
flag is present and neither join flags nor a local function prohibit that
deletion. A function inside a child SELECT does not cross the local expression
boundary. Parameters were already assigned at their source occurrences.
`sqlite3PExprIsNull` skips ordinary unary signs, recognizes INTEGER/REAL/TEXT/
BLOB syntax and constructs an integer boolean without materializing its value.
COLLATE and likelihood wrappers are barriers there.

Completed controls distinguish raw0/hexadecimal0 from unary-signed0,
QNUMBER0_0 and the unresolved FALSE keyword; deleted names/child queries/
scalar widths from retained local functions; retained parameter metadata and
invalid parameter errors; ordinary statements from otherwise identical IN
contexts; and null-test literals from wrapper barriers. Existing eager VALUES
errors still survive an enclosing discarded AND or OR.

Modern already has the canonical translation in `SyntaxNormalization`,
`NormalizeExpressionConstruction`, the shared integer/function-presence
helpers and the postorder immutable syntax arena. ADR-0152 allocated that
annotation only when membership existed; ADR-0159 extended it to multirow
VALUES. `AssignParameters` still gates the ordinary pass on `has_membership`.
The transformations must not depend on an unrelated membership expression.

## Decision

### Reuse one constructor annotation and kernel

During the existing parameter/expression inventory, detect the current
constructor consumers: IN, logical AND and IS/IS NOT syntax. Select the same
ordinary construction pass when any is present. Rename the membership-only
pass to describe expression construction; add no second walker, truth
classifier, semantic AST, parser or annotation container.

Keep the existing source-ordered parameter assignment and the existing
multirow VALUES event schedule. Do not bind a deleted operand, successfully
bind a value twice, drop a constructor-owned VALUES error or fabricate NULL.
The per-expression construction kernel, zero/boolean identity, collation
provenance, local-function boundary, cached heights and later binding consume
the same annotation in every context.

Statements without these constructor candidates retain their current ordinary
allocation path. Candidate statements intentionally use the already canonical
O(expression-count) annotation rather than putting mutable semantic flags in
the pure source-preserving AST. This extends ADR-0152's eligibility restriction,
not its implementation or the dependency DAG. Keep the ordinary bound-expression
and lowering-error footprints unchanged.

Validate literal token/kind structure through the existing canonical validator
before a constructor can delete it; lexical validation must not parse a64-bit
value or allocate BLOB bytes. Reuse that validator for constant defaults with
their existing bare-name form. CREATE TABLE construction retains that admitted
default-token form; ordinary expressions do not acquire a new bare-string rule.
Malformed public AST literals must not turn into success merely because a
valid false operand discards them.
Double-quoted string-kind public AST nodes remain name-bearing until binding:
exclude them from constructor-time literal null-test folding. Preserve nullable
column lookup, the DQS-disabled name error and the enabled unresolved-text
fallback through the same canonical operand classifier.

Preserve invalid-input validation, function-constructor limits, parameter
metadata, intermediate construction-height checks and typed errors. This
changes neither limits nor the existing parameter/construction scheduling
algorithm; it does not claim to repair every diagnostic-order distinction.

### Keep the next value-generation boundary separate

This slice does not turn later OR simplification into parser-time name deletion.
`1 OR missing_name` must retain the name error even though a value-only bad
literal can be ignored natively. Local function name/arity checks remain
required when they prohibit constructor-time deletion.

The executed ordinary logical-value and required-unary findings remain blocking
follow-ups: `1 OR 0x10000000000000000`, name-before-value diagnostics and direct
negation of hexadecimal INT64_MIN require their actual value-generation phase.
Do not add an early unary guard that regresses native name or pruning order.
The current lowerer logical simplifier and shared literal materializer remain
the canonical mechanisms for that later design; do not duplicate them here.

No SELECT/VALUES IN RHS, new statement kind, VM capability, storage transition,
benchmark family or later DAG node is admitted.

## Test-first, ownership and delivery

Record a focused public runtime red for ordinary false-AND name deletion before
production changes. Add paired ordinary/IN controls for false flags, null tests,
local functions, child-query name/width deletion, parameter metadata and wrapper
barriers. Keep earlier VALUES-constructor errors, malformed descriptors,
maximum-depth and invalid-variable controls.

Exercise read and write consumers through the same binder. Cover source-owner
moves, reset/rebind, statement failure/rollback and complete owning allocation
failure sweeps without changing ceilings. Check that plain noncandidate SQL
keeps its no-annotation allocation path and candidate work remains one bounded
canonical annotation. Retain all assertions, workloads and process deadlines.

Review the design before implementation. Each coherent commit requires actual
red/green, complete fast units, implementation review and node/ADR ledger
traceability. Run the actual change-aware closing plan and necessary
native/compiler/sanitizer/fuzz gates against the exact final head, then guarded
squash delivery. Node43/project completion and whole-engine parity are not claimed.

## Recorded implementation evidence

The public ordinary false-AND regression failed with `no_such_column` before
production changes and now returns the native integer0. Independent design
review accepted this bounded scope before implementation.

A corrective malformed-public-AST regression then exposed that a false
constructor could hide a mismatched literal kind. It failed before the
lexical-before-deletion guard and now passes through the canonical validator.
Bare CREATE TABLE default names and the existing unsupported default-expression
boundary remain covered; value-range parsing is not part of this guard.

Independent implementation review also caught double-quoted string-kind public
AST nodes being folded before name lookup. A focused runtime red confirmed
that nullable-column resolution disappeared; the canonical null-test classifier
now excludes those name-bearing tokens. Pinned native enabled/disabled DQS
controls independently retain nullable-column results, the enabled text fallback
and the disabled missing-column error.

The expanded25-case focused selection and complete2063 native isolated unit
selection pass, including the original depth/function-construction controls,
parameter/source-owner lifecycles and shared mutation rollback. The binder's
complete allocation-failure population grows40 to44 scenarios under its
unchanged1024 ceiling.

The existing subquery shard retains284 historical cases and adds25 bounded
construction cases, for309 total below the unchanged512 cap. Genuine pinned
GNU regeneration preserves every historical native outcome and compiler
provenance; all307 comparison cases and both unsupported controls pass against
the current Modern trace. These observations do not replace implementation
review or final-head native/compiler/sanitizer/quality/fuzz acceptance.
