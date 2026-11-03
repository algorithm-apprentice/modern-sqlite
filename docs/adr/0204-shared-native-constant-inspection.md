# ADR-0204: Shared Native Constant Inspection

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted ADR-0203 source209 `0cd35544`, ledger209, draft PR #292
- Related contracts: ADR-0106/0111, ADR-0160/0162/0164 and ADR-0190/0191

## Completed source mapping

Sources208/209 close the ordered verification-fixture prerequisites without
changing the production engine. Resume the separately preserved inspection
extraction; its previous code review and partial qualification were not source
acceptance. Its cancelled GNU build receives no qualification credit.

The original qualified-main trace was explicitly rebuilt before65 research
observations:30 ordinary unary,14 ordering/grouping,15 generation-order and
six function-construction-barrier controls. Their outcomes and diagnostic text
retain known differences; they are not a general-generation parity claim.

The next generation owner needs native constant inspection rather than a
statement-guard predicate or recursive function-presence flag. The pinned
SQLite mechanism is:

| Responsibility | Native source |
|---|---|
| Scalar determinism and recursive argument inspection | `expr.c:2485-2532` |
| Constant, non-IS and outer-ON-sensitive policies | `expr.c:2535-2658,2677-2712` |
| BINARY GROUP BY pruning before constant inspection | `expr.c:2838-2888` |
| Temporary operand wrapper stripping and inspection | `expr.c:5980-6002` |
| Function-bearing versus deferred generation after inspection | `expr.c:5901-5947` |

Modern already implements the structural mechanism in
`StatementBinder::ConstantExpressionControl` and `InspectConstantExpression`.
It uses borrowed bound expressions/functions, `OrdinaryExpressionChild`,
`ExpressionWalkControl` and `compiler_detail::ContinuationStack`. Current
consumers are reusable IN-list classification and constant-or-BINARY-group
HAVING classification. `BoundOrdinaryChild` also serves the existing value
validation walk and must remain available there.

Traversal is independent of name resolution and bytecode. Its policies are
not: the binder owns group-term equivalence, and a later generation consumer
must use the actual effective/retained ON owner. Construction
`has_function_flag` selects a subsequent generation owner, not whether a
function and its arguments are constant. A COLLATE construction barrier does
not make a nondeterministic function constant.

## Decision

Move this existing inspection kernel, not a second implementation, into one
private binder header. Retain node classifications, child ordering, pruning,
short-circuit behavior and tail descent. Reuse the same continuation container
and borrowed expression/function spans. Add no annotation, memoization table,
register, value or public bound-IR field.
Register the new private header in the existing explicit CMake format-input
list so the canonical formatting target continues to cover this implementation.

Keep the current inspection modes. Supply the existing BINARY-group pre-node
decision through a nonowning observer called before structural classification.
Its `Continue`, `Prune` and `Abort` meanings are the existing
`ExpressionWalkControl` contract. The binder's observer applies the existing
`MatchesBinaryGroupTerm` only in its current group mode; other modes continue
unchanged. Preserve group pruning's priority over ordinary column/function
rejection.

The kernel consumes trusted bound IDs produced by the binder, not unvalidated
public syntax or serialized input. Keep this precondition explicit with
existing-style assertions at expression/function access. A later observer's
typed failure must be retained and surfaced by its caller as in the current
ON-dependency inspection, not converted into successful nonconstancy.

Keep group equivalence and query/join metadata with callers. Add no unused
outer-ON mode, statement-guard substitution, recursive rediscovery of
construction flags or traversal into child queries as ordinary constants.

This is a green-refactor prerequisite for the actual generation owner. It
changes no literal materialization, scalar-width error, constructor schedule,
function execution count, constant initialization, physical pruning or unary
arithmetic. The subsequent behavioral source still requires completed caller/
state mapping, accepted design and a genuine focused red.

## Verification and delivery

Use the authenticated current-source green predecessor as the refactor
baseline. Do not invent a failing behavior test for unchanged behavior.
Run owning binder cases, including deep membership/group inspection, aliases,
function flags and complete allocation-prefix failures, then the complete
fast tier. Preserve all original stack, depth, allocation and timeout limits.

Reconcile all65 semantic controls without erasing their known differences;
outcomes and diagnostic winners must be unchanged. Verify public carrier
sizes and the unchanged inspection allocation mechanism. Execute the actual
change-aware native, genuine GNU/sanitizer and production-analysis plan.
Use explicitly built current trace targets; paused or incomplete builds are
not reusable qualification.
The format registration changes the root CMake file and therefore selects the
unchanged policy's full verification plan. Do not downgrade that escalation.
Reconcile unchanged compiled inputs, images and test definitions after
configuration before reusing prior execution. Use the existing native
canonical formatting target with its installed formatter when the unrelated
cached Unix wrapper cannot launch; keep all formatting inputs and strict flags.

Review this renumbered design before restoring the two owned implementation
files. Independent implementation/evidence acceptance precedes a normal
source210 commit and append-only ledger. This remains the third bounded source
in the related draft batch, not a general-generation repair.

Exact-final-head CI and integrated review must pass before squash merge.
Qualify the resulting actual-main commit after merge and before branch cleanup
and delivery closure. General generation, IN query RHS, node43 and the project
remain incomplete.

## Source acceptance and final closing

Use ADR-0106's distinction between an independently accepted source and the
completed delivery batch. Source210 requires its accepted design, authenticated
green-refactor baseline, owning focused and complete fast-unit evidence,
implementation review, current public-layout and actual production-sanitizer
evidence, and an append-only ledger record before a successor source begins.
Keep the actual full verification plan and its execution provenance explicit.

The attempted native closing selection contains 58 configured owners. Nine
completed before the read-benchmark CLI owner exceeded its unchanged
300-second deadline; 48 owners were not executed. Preserve that failure and
do not promote it, diagnostic observations, or unexecuted owners to successful
qualification. Matched data-only location observations retain identical
logical work, but do not uniquely attribute the original timeout or establish
a general engine speed result.

If implementation review confirms that this inspection refactor remains
independently coherent, its normal source commit may precede one separately
designed and reviewed fixture-ownership repair under ADR-0106's four-source
batch bound. The repair starts only after source210 is SourceAccepted,
committed, and recorded in the ledger. It may not relax case populations,
iterations, assertions, diagnostic limits, cache or durability settings,
allocation bounds, deadlines, or production benchmark behavior.

Keep PR #292 draft and unmerged. Source acceptance is not batch completion:
all outstanding full-plan native owners, required final-head local and hosted
gates, and integrated review must pass on the final exact batch head before
squash merge. Retain prior failures and only reuse evidence whose relevant
inputs and execution identities match. Any repair that changes an earlier
source's contract or consumed implementation invalidates its dependent suffix
as specified by ADR-0106. Qualify resulting main before cleanup and closure.
