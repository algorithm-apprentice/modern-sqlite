# ADR-0245: Demand-Owned Scalar Argument Ranges

- Status: Accepted
- Date: 2026-11-10
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #318 main `30986ddf`, ledger338
- Related contracts: ADR-0036/0106/0111/0216/0218/0234/0237/0238/0240/0244

## Requirement and completed bounded inventory

The shared initial register layout reserves an argument block for every
bound scalar call, including calls whose values are never generated.
Private emission separately obtains its checked owner-specific block, so a
private-only call also retains an unused main block.

Native allocates function argument ranges when `sqlite3ExprCodeTarget`
actually generates the function. An ordinary EXISTS destination omits its
result expressions; it therefore does not request their function argument
ranges. Generation demand is not runtime reachability: a generated call
behind a false input guard or an unreached conditional arm still needs its
arguments.

Twelve fresh paired controls on qualified main338 preserve native/Modern
rows and callback counts, with no preparation callback. The current program
for `SELECT EXISTS(SELECT deterministic_trace(7)) FROM items` emits no scalar
call but declares nine registers and rejects an eight-register limit. Its
bound arena contains four expressions and one scalar argument slot.
The same ignored-result shape with a two-argument function and two nested
scalar calls reserves four argument slots despite emitting no scalar call.

Ordinary, reached-once, nested, false-input, conditional, private-query,
original VALUES, shared-alias and disabled-membership controls retain their
current behavior and exact register-limit failures. The observations record
program resources and fixed work, not a timing improvement or a TDD red.
The initial observer's size_t formatting warning is preserved; its corrected
version retains the strict native warning profile.

| Responsibility | Canonical source |
| --- | --- |
| Actual scalar generation and argument allocation | `sqlite3.c:118295-118340` |
| Omit ordinary EXISTS result generation | `sqlite3.c:150690-150712` |
| Native temporary-range ownership and callable separation | `sqlite3.c:120501-120560` |
| Initial expression homes and argument blocks | `BeginExpressionRegisterLayout` |
| Canonical scalar/IN range width | `ExpressionArgumentCount` |
| Checked late range allocation | `ProgramBuilder::AllocateRegisters`, ADR-0216 |
| Shared main/private binding selection | `ExpressionRegister`, `Home`, `ArgumentBlock` |
| Real scalar and membership requests | `BeginScalarEmission`, `BeginMembershipEmission` |
| Physical owner and contiguous private definitions | ADR-0218, `ExpressionRegisterOwner` |
| Existing exact-limit and ownership controls | `plan_lowering_test.cpp` |
| Existing full allocation-prefix population | `plan_lowering_oom_test.cpp` |

## Decision

### Reserve scalar arguments only at an actual emitter request

Keep the initial main expression-home prefix, parameter counts, literal and
symbol ownership, query layouts and all non-scalar argument requirements.
In particular, preserve the current three-register membership scratch
layout. Do not remove the initial expression-indexed optional binding table
or predict requests with another syntax traversal.

Stop assigning initial main argument ranges to `BoundScalarCallExpression`
nodes. When `ArgumentBlock` is actually requested in main, lazily obtain the
checked contiguous range and cache it in that same existing optional slot.
Reuse that slot on later requests in the same main region.

Keep private owner selection and its lazy binding table unchanged. Reuse
one canonical ensure/allocate implementation for both main argument slots
and private home/argument slots; do not copy the existing allocation branch
or add a second counter, container, arena, register pool or value cache.
Continue obtaining widths through `ExpressionArgumentCount`.

The builder remains the sole owner of emission-time register growth.
Successful allocation does not initialize a value. Preserve empty-range
semantics for zero-argument functions, checked identity/configured limits,
typed errors and normal compiler allocation unwinding.

Retain the initial `FinishRegisterLayout` barrier for the remaining prefix
and its existing register-before-parameter checks. A later scalar range can
fail only at its actual checked request. Its cause remains the existing
`kRegisterLimitExceeded`/lowering resource error; no partial program is
published and no missing range becomes register0.

### Preserve generation and physical lifetime

Do not change whether an expression is generated, where its code runs,
which concrete child result its parent reads, or which physical owner
contains an instruction. Keep all existing fixed/preferred policies,
factoring modes, once sites, original VALUES replay and grouped ownership.

Generated-but-runtime-unreached calls still allocate their arguments.
Omitted EXISTS results do not. Private-only calls keep their existing
private range without a speculative main copy. Aggregate argument layouts,
membership storage, coroutine outputs and captured/representative values
remain under their existing owners.

Main result/control registers can receive earlier IDs and actual scalar
argument ranges can receive later IDs. All emitted references use the
existing typed bindings; no relocation pass or instruction rewrite is
introduced. Preserve instruction kinds, evaluation order, result types,
callbacks and lifetime behavior.

This narrowly supersedes ADR-0218's retained initial scalar-argument layout.
It does not add native constMask/fixed-factorable persistent destinations,
per-generation retained argument identity, temporary-register pooling,
FIFO initialization or an unused-original-producer pass. Those requirements
remain separate; this source removes preallocation that would otherwise
compete with actual demand ownership.

## Test-first proof and bounded delivery

After independent design acceptance, add one owning regression for the
observed ignored scalar result under its eight-register budget and record
the current typed rejection. Then make the smallest shared allocation
change and prove successful verified-program publication.

Retain a boundary control for a genuinely generated call behind an empty
input, together with the existing exact-limit private/query/aggregate
controls. Cover zero-argument, nested, read/write, original-site/alias,
owned TEXT/BLOB, failure/reset and mixed-depth consumers through the
existing owning tests. Add focused cases only for newly uncovered behavior.
Keep the complete lowerer allocation-prefix population and all ceilings,
workers and deadlines.

Repeat the twelve matched program/resource observations against the actual
candidate. Account for removed unused ranges separately from instruction
and callback work; do not infer a heap-byte or speed improvement from a
register count alone. Retain the broader existing deferred/original-site
and preparation controls, including their unresolved differences.

Run focused green, complete fast units, current strict compiler/checker
owners and the actual change-aware closing plan. Independent source review
and normal node/ADR/ledger traceability precede any dependent source.
Exact-head hosted/integrated and separate actual-main qualification remain
mandatory. The nine deferred-callback and two unrequested-constructor
differences, general error arbitration and IN remain incomplete.
