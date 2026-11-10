# ADR-0242: Preferred Expression Result Registers

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #316 main `2ea97ddc`, ledger329
- Related contracts: ADR-0106/0111/0218/0219/0221/0226/0231/0234/0237

## Requirement and completed bounded inventory

ADR-0231 transports an actual child result through the existing continuation,
but still requires every value completion to equal its requested destination.
ADR-0234 consequently copies every reached-once result into expression scratch,
even when the parent only reads that operand. The original VALUES callable
adapter has the same forced-copy boundary.

Native has both required and preferred destinations. `sqlite3ExprCode`
guarantees its target; `sqlite3ExprCodeTarget` and `sqlite3ExprCodeTemp` may
return a different register. Their factoring policies differ, even though
both result contracts allow that return. A preferred destination must not
silently become permission to defer or factor every expression.

The pinned native caller inventory was reconciled against the unchanged
amalgamation SHA256
`4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6`.
The following mapping bounds this result-contract migration:

| Responsibility | Canonical source and current consumer |
| --- | --- |
| Retained function result and fixed/temporary wrappers | `sqlite3.c:118257-118267,118698-118740,118776-118849`; `BeginReachedOnceEmission` |
| Arithmetic, general negation, unary and truth operands | `sqlite3.c:118095-118220`; `BeginValueEmission`, `DescendHome`, `ResumeEmission` |
| Reordered/null-skipped comparison operands | `sqlite3.c:115232-115278,119104-119165`; `BeginComparisonBranchEmission` |
| Logical first/selected target versus second temporary | `sqlite3.c:117770-117835`; `LogicalCoercion`, `QueryLogicalFirst`, `QueryLogicalSecond` |
| Generic condition value | `sqlite3.c:119159-119169`; `BeginBranchEmission` |
| Linear IN RHS temporary, disabled LHS and mutable RHS cells | `sqlite3.c:116921-117020`; `NextLinearMembership`, `NextMembershipBuild` |
| Forced scalar argument and conditional result blocks | `sqlite3.c:117483-117507,118335-118340,118886-118925`; `NextScalarArgument`, coalesce and conditional continuations |
| Transparent positive/COLLATE/likelihood dispatch | `sqlite3.c:118435-118455`; existing value-emission loop |
| Original VALUES callable identity and result lifetime | ADR-0237; `OnceGenerationTraversal`, `OnceExpressionSite`, `EmitSubroutineValue` |
| Concrete child result and earlier-operand retention | ADR-0231; `EmissionComplete`, `EmissionFrame`, `EmitOperationLoop` |
| Read-only arithmetic, truth and comparison inputs | `vm.cpp:1084-1135,1210-1285,1333-1380` |
| Canonical allocation, protected construction and cleanup | `Home`, `AllocateReachedOnceSite`, ADR-0218/0219/0221/0226 |

The existing VM operations read their inputs and write their distinct declared
outputs. Comparison affinities operate on local converted values, not the
retained input. Membership construction instead explicitly applies affinity
to its key cell; that destination must remain fixed.

Query results and aggregate representatives have separate mutability and
generation identities. They are not made borrowable by this decision. The
external `EmitExpressionAtHome` service also retains its fixed-result contract;
its range/key, mutation, projection and aggregate callers are not blanket
migrated merely because their method name contains `Home`.

## Decision

### Distinguish required and preferred destinations in one emitter

Add a small compiler-local destination policy to the existing
`EmissionRequest`: fixed or preferred. This describes the returned-register
contract only, not native temporary/factorable scheduling.

Fixed requests retain their current destination and completion checks.
Preferred requests must return an actual value register, but it may differ
from their proposed scratch. Branch and membership-control requests still
complete without a value. A wrong completion kind or a mismatched fixed
destination remains a typed internal-invariant error.

Keep the existing iterative continuation, concrete child-result transport
and phase-owned operand registers. Add no public carrier, result table,
register container, runtime borrowing representation or alternate emitter.
The public/internal service-facing value entry points remain fixed.

Rename the internal home-selecting descent to make its preferred-result
contract explicit. It still obtains scratch through the same `Home` owner;
do not add temporary-register pooling, reclaim that storage or change its
physical ownership in this source.

Use preferred child results for arithmetic, general unary, truth, comparison,
logical-value coercion, generic branch values and linear IN RHS operands.
Preserve the first operand across generation of the second, including native
reordering and NULL/short-circuit edges. Unary positive and transparent
wrappers preserve the enclosing destination policy.

Keep scalar argument blocks, coalesce and conditional arms, IN LHS values,
membership construction cells and reached-once bodies fixed. Preserve the
disabled factoring mode of IN LHS and once bodies. A preferred consumer never
mutates, moves, applies in-place affinity to or publishes a retained result
as a mutable coroutine output.

### Return only the existing invocation-stable once result directly

An admitted reached-once function still owns exactly the same fresh result,
gate, actual generation identity, disabled body and entry NULL construction.
At completion, return that register directly to a preferred consumer.
Fixed consumers retain the existing copy into their required destination.

Apply the same contract to original VALUES once callables. Reuse the current
call-and-result adapter; a requested result equal to its retained source
needs the call but not a self-copy. Do not remove ordinary subquery result
copies or infer that mutable query, representative or scratch registers
have the same lifetime.

The result is stable throughout one statement invocation. Existing gate,
callable ownership and terminal failure/reset cleanup remain authoritative.
Shared original-site replay and grouped-site replay keep their existing site
sequence. This changes neither site admission nor callback reachability.
Preserve construction flags, contextual representative selection, effective
outer-ON ownership and source/name/value/error decisions.

Do not grant new bytecode ownership or initialization exceptions. The
constant-only entry proof remains unchanged; no parameter, arithmetic or
function execution moves there. No VM, binder, planner or public-type change
is needed.

### Keep the remaining generation boundary explicit

This is a behavior-preserving result-contract migration with measured
instruction/copy changes, not a complete native expression generator.
It does not add whole-root factoring, a reusable-expression search, fixed
factorable destinations, FIFO initialization, unused original-producer
generation or preparation-error accumulation.

The native target-versus-temporary factoring distinction remains a required
input to the next scheduling design. In particular, logical first operands
use CodeTarget rather than CodeTemp; preferred does not identify which one
was requested. Retain the nine deferred-callback and two unrequested
constructor differences explicitly. No IN query RHS or later DAG node is
admitted.

## Baseline, proof and bounded delivery

Obtain independent design acceptance before production changes. Start from
the qualified green implementation and complete fast-unit evidence, matching
its actual source, configuration, executable and raw report identities before
reuse. Do not manufacture a failing behavior test for a refactor.

Run the existing owning arithmetic/noncommutative, NULL, logical, comparison,
conditional, IN, grouped/original-site, mutation and mixed-depth controls.
Add only missing focused lifetime coverage for a retained TEXT/BLOB value
read as an operand across coroutine suspension, independent VMs, failure and
reset/rebind. Fixed argument and mutable-key controls must remain covered.
Keep the complete lowerer allocation-prefix population and all ceilings,
workers and deadlines.

Record a matched preparation/allocation and generated/executed-work baseline
before production changes. Reuse existing observer mechanisms and rebuild
against consumed definitions. Report removed copies and any changed compiler
allocation, while retaining site/register/callable counts and complete
retirement. Make no timing or speed claim.

Retain the existing deferred/original-site and preparation protocol populations,
including their unresolved differences. Run focused green, complete fast
units, current strict compiler/checker owners and the actual change-aware
closing plan. Independent source review and normal node/ADR/ledger traceability
precede a dependent slice. Exact-final-head local/hosted/integrated and separate
main qualification still govern delivery.
