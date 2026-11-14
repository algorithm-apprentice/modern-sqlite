# ADR-0259: Canonical Ordinary Construction Events

- Status: Accepted
- Date: 2026-11-14
- Module graph node: `implement-advanced-sql`
- Prerequisite: PR #333 qualified main `71c77ed0`, delivery ledger402
- Related contracts: ADR-0102/0106/0111/0159/0161/0191/0257

## Confirmed boundary and existing owners

ADR-0257 makes SELECT enter the common initializer before compound admission.
The initializer still has two construction paths. Multirow VALUES interleaves
parameters, expression constructors and completed rows through
`NativeConstructionEvent`. Ordinary statements assign every parameter first,
then normalize expression constructors only if IN, a subquery, AND or IS
selects that pass.

Current-source pinned comparisons establish two consequences. A function over
the configured argument limit can lose to a later missing source, name or
compound-width error when no other expression selects normalization. Even
when normalization is selected, a later invalid parameter can hide that
earlier function constructor. The equivalent multirow VALUES controls already
retain native order.

The function constructor also counts the AST wildcard as an ordinary argument,
whereas `BeginFunctionBinding` already treats a wildcard call as zero arguments.
With a zero function-argument limit, pinned SQLite accepts `count(*) AND 1`
and its FILTER form. The current ordinary constructor rejects them. This is
an argument-list classification discrepancy, not permission to change
function lookup, aggregate semantics or expression-height rules.

The oracle is the locally pinned SQLite 3.54.0 source and matching native
library, not the current online release. The ordinary comparison profile
limits function arguments to1000. Its variable limit is256, unlike Modern's
default32766; compare error ownership rather than asserting identical raw
messages for those unequal profiles. Explicit zero/two-argument controls
configure the same requested limit.

## Completed source map

| Responsibility | Canonical source |
|---|---|
| Native function allocation, list limit, flags and allocation cleanup | `sqlite3.c:113980-114008`, `sqlite3ExprFunction` |
| Native bare-star empty list | Function grammar reductions191/194 at`sqlite3.c:185211-185238` |
| Argument, ORDER and FILTER children before their enclosing constructor | Reductions189-194 at`sqlite3.c:185200-185242` |
| Variable assignment at its grammar reduction | `sqlite3.c:185163-185170`, `sqlite3ExprAssignVarNumber` at114127-114204 |
| Parse diagnostic ownership, next-token stop and parser cleanup | `sqlite3ErrorMsg` at37060-37083; `sqlite3RunParser` at187453-187575 |
| Existing native row transitions and eager/fallback ownership | ADR-0159's completed `parse.y`/`insert.c`/`expr.c` map |
| Source-preserving function children and final span | `Parser::ParseFunctionCallTail`, `ParseFunctionCallSuffix`, `FinishFunctionCall` |
| Validated preceding children, unique ownership and contained spans | `SyntaxTree::Create`, `ReferenceExpression` |
| Common read/write initialization | `StatementBinder::InitializeCommon`, `AssignParameters` |
| Constructor annotation, literal/depth checks and deletion facts | `SyntaxNormalization`, `NormalizeExpressionConstruction` |
| Lexical construction events and row-owner bootstrap | `NormalizeValuesConstruction`, `NativeConstructionEvent` |
| Existing zero-argument star classification and function limit error | `BeginFunctionBinding`, `ValidateFunctionArgumentCount` |

FILTER is part of the native function reduction: an invalid parameter or
over-limit function inside FILTER precedes the enclosing function's limit
check. ORDER children likewise precede their function. An earlier completed
function precedes a later result parameter. Sibling expression IDs alone are
not a lexical-order contract for a structurally valid public AST.

Constructor validation precedes subsequent false-AND/empty-IN deletion,
ordinary name binding and overwritten mutation-value selection. It does not
perform those later phases early. Native internal `Parse::nested` construction
is distinct from a normal nested SELECT; no new internal parse mode is
admitted. The immutable parser remains pure syntax.

## Decision

### 1. Share the existing wildcard argument classification

Extract the existing binder predicate that detects a function's wildcard
argument. Use it in ordinary function binding and constructor argument-limit
admission. Both interpret the existing wildcard representation as an empty
native argument list; do not rewrite the AST, allocate another list, special
case the spelling `count`, or add a public flag.

Keep unknown-function, wrong-arity, DISTINCT, ORDER, FILTER and aggregate
validation at their existing owners. Preserve native zero-argument behavior
without treating `count(1)` as zero arguments. Construction heights and the
separately observed flat-depth boundary remain unchanged and open.

### 2. Use one event owner for selected construction

Include function-call syntax in the existing construction-eligibility
predicate. Decide eligibility before allocating or assigning ordinary
parameter occurrences. A selected ordinary statement uses the existing VALUES
event scheduler, renamed to describe shared construction. Remove the separate
ordinary postorder normalization loop.

The shared scheduler retains its checked event count, expression/row records,
source-end ordering, child-ID tie break, expression-before-row tie break,
parameter assignment kernel and expression-construction kernel. Each
parameter is assigned at its event, once. This is not a new function scan that
publishes errors independently of other constructors.

Preserve `InitializeCommon`'s multirow distinction: query arenas exist before
an eager VALUES row can enter binding. Ordinary construction without multirow
VALUES cannot create such rows and needs no fabricated query identity.
Existing first/incoming row preparation, producer chunks, cached roots,
schema-check provenance and scoped cleanup remain unchanged.

Statements with no constructor candidate retain the parameter-only path:
the same lexical occurrence sort, assignment state and no construction
annotation/event allocation. The eligibility inventory introduces no new
container. Selected statements use one annotation and one existing event
array, not an ordinary parameter array followed by another schedule.

This extends ADR-0159's accepted immutable-arena transposition to ordinary
construction. Its cost is explicit: for E expressions and R multirow events,
O(E+R) temporary storage and O((E+R)log(E+R)) sorting. Native grammar performs
streaming reductions; this architectural translation preserves their order
without recoupling parser and binder. Record before/after allocation and
fixed-work preparation observations for ordinary, function, parameter and
VALUES controls. Keep the existing allocation ceilings and matched closing
contracts. No speedup or constant-cost claim is made.

Semantic failures remain existing typed errors. Allocation failures unwind
the same binder-owned objects through its existing allocation boundary.
Do not continue after a failed event, erase a constructor failure, or publish
partial parameter metadata as a successfully bound statement.

## Two reviewed increments and proof

The first coherent commit shares wildcard classification and repairs the
zero-limit boundary. Its focused red/green covers both SELECT binding facades,
ordinary and already-normalized stars, FILTER, zero-argument calls, a real
one-argument rejection and wrong-arity controls. Add the configured-limit
case to the existing complete allocation-failure sweep.

After that commit is accepted, the second admits ordinary functions to the
shared event owner. Cover source/name/compound precedence; both orders of
function and parameter errors; nested function, FILTER and ORDER children;
existing eager VALUES precedence; shared mutation entry; and valid parameter
names/slots through discarded expressions and owner moves. Retain malformed
AST, constructor deletion, function flags, depth and schema-check controls.

Every increment needs accepted design, actual focused red/green, complete
fast units, owning allocation/lifetime evidence, independent implementation
review and append-only ledger evidence before the next begins. Use one
related two-commit PR under ADR-0106, then the actual final change-aware
closing union, exact-head hosted gates and descendant-main qualification.

This batch does not close source expansion, `NC_NoSelect`, native query-width
phases, expression-height discrepancies, all constructor diagnostic ordering,
IN admission or node43. Those remain explicit subsequent work.
