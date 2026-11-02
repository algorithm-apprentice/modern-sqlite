# ADR-0190: Canonical Native Logical Selection

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified ADR-0189 / PR #282 main and ADR-0163/0164
- Related contracts: ADR-0102, ADR-0106, ADR-0111 and ADR-0160/0161

## Completed source mapping and current requirement

PR #268 delivered recursive native truth selection and composable compiler
continuations: source `027cccb3`, squash `d5ef9694`. That squash is an
ancestor of PR #282's main `f4d97ac9`, tree `e56f5135`. Historical pending
audit wording does not reopen the delivered truth correction. The separate
unmerged PR #269 contains diagnostic work and is not truth-delivery evidence.

The remaining literal/name/constructor/unary/scalar-width/collation/default
findings still block IN query RHS. Direct pinned-source and paired execution
mapping shows that constructor value preparation and later expression
generation both need the already implemented native logical selection.
For example, the constant VALUES row in
`SELECT 1 OR (VALUES(1 OR 0x10000000000000000),(1))` is accepted natively;
Modern rejects its unused literal while preparing the constructor.
Ordinary names and early constructor-owned values remain different phases.
This ADR does not authorize replacing either with parser-time deletion.

The pinned `expr.c:2391-2402` selector recursively observes the right child,
then the left child, and chooses an existing operand only from native
always-true/always-false properties. It does not evaluate arbitrary constants
or edit either child. `expr.c:4994-5000,6239,6436` shares that selection
between value and conditional generation.

Modern already has this exact algorithm in
`PlanLowerer::SimplifiedLogicalOperand`, with real-alias peeking in
`LogicalOperandBinary`, the existing `BoundTruthHint` and
`compiler_detail::ContinuationStack`. Value and branch continuations call
the same selector. `NativeTruthHint` separately observes effective outer-ON
retirement through the active physical core and retained origin metadata.
Its phase-specific observation must not move into a context-free binder
rule. Executed preserving/reduced-join controls demonstrate that distinction.

## Decision: one behavior-preserving foundation

Move the existing selector and its alias-peeking operation into one internal
bound-expression helper, `src/binder/native_truth.hpp`. Keep the existing
bound arena, expression IDs, truth hints and continuation owner. Do not
add a second selector, context registry, expression property cache or tree.

The helper borrows a stable span of bound expressions and the current binary
root. A concrete callable supplies the existing typed truth observation for
an expression ID. Follow the established callable pattern of the shared
bound dependency helpers; use no `std::function`, virtual interface or
allocation for callback dispatch. Preserve the observer's typed error.
This is the one varying phase input, not a configurable rewrite framework.

Retain the exact Right/Left/Observe continuation order and pending-frame
transitions. Peek through actual alias links only to identify a nested
logical binary; if that child does not simplify, retain its original ID
and wrapper. COLLATE, unary and likelihood barriers are not newly transparent.
Return the selected existing operand or no selection, never an invented
boolean value or a rewritten subtree.

The caller still owns boolean coercion, conditional destinations, query
flags, representative registers and bytecode generation. Keep
`PlanLowerer::NativeTruthHint` in the lowerer with all existing active-core,
ordinal and effective-versus-retained ON checks. A small forwarding method
may adapt that observer to the shared helper; remove the old algorithm and
alias walker rather than retaining a recursive or local fallback.

Input references retain the current trusted bound-arena invariant. The
helper neither appends expressions nor retains views after returning.
The existing inline-first continuation stack remains the only work owner;
deep overflow and its allocation failure propagate unchanged. No new
depth bound, exception policy, storage state or public program flag is added.

## Acceptance and ordered continuation

This source is a behavior-preserving extraction, not a claim that the
remaining value-phase defects are fixed. Start from the completed green
ADR-0189 baseline after actual-main qualification. No artificial failing
test is required.

Retain existing recursive truth, alias and ON-retirement controls, including
skipped IN descendants; conditional/value wrapper barriers; exact logical
and mixed-expression default-stack boundaries; query and statement moves;
and complete owning allocation-failure coverage. Run the focused selection,
complete fast units and the actual change-aware closing plan with the
unchanged compiler, sanitizer, quality, workload and deadline requirements.
Independent source review and append-only evidence precede a normal commit.

A subsequent constructor or ordinary value-preparation integration requires
its own accepted owner/ordering design and genuine behavioral red before
implementation. It must reuse this selector, the existing owner-backed
literal token/materializer, child enumeration and compiler continuations.
This foundation does not authorize a new evaluator, duplicate bind pass,
global value-demand bitmap, emission-only deferral that loses WHERE0 errors,
or a binder-side guess at effective join reduction.

Related accepted sources may share an ordered ADR-0106 draft; every prefix
must pass its own required gates before its successor begins. Final exact
union qualification and review precede squash delivery. Node43 and IN remain
blocked until all confirmed prerequisites are closed.
