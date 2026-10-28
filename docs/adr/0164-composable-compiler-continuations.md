# ADR-0164: Composable Compiler Continuations

- Status: Accepted
- Date: 2026-10-28
- Module graph node: `implement-advanced-sql`
- Prerequisite: ADR-0163's accepted provenance and join transition
- Implementation: canonical continuations; exact-source qualification and review pending
- Shared dependency-walk refinement: Accepted before implementation at proposal `6ff54e93`
- Overflow growth refinement: Accepted before implementation at proposal `3393fa04`

## Context and reproduced failure

ADR-0028 defines two independent bounds: construction height1000 and512 active
recursive parser calls. ADR-0153 retains the ordinary native Windows stack.
Passing each pure expression family separately does not establish a bound for
combinations admitted by both limits.

Independent source review rejects uncommitted ADR-0163 tree `6ef5b616`.
The parent reproduces the exact control against the pinned native MSVC oracle:

1. Start with `?1` followed by480 `+0` operations.
2. Wrap that expression in500 `iif(expression,1,0)` calls.
3. Prepare the resulting SELECT and bind parameter2.

The construction height is981 and the parser call bound is not exceeded.
Native preparation succeeds and returns integer1. Modern parses the same SQL
but ends during binding on the unchanged1MiB stack. Separate400/400 and480/200
controls reach the physical plan and end during lowering for conditional and
ordinary `abs` wrappers. Pure500-wrapper and pure980-arithmetic controls pass.
These are actual phase observations, not deductions from frame sizes.

The rejected tree passed22 focused tests,2111 isolated units, a complete2146
native-selection union, strict GNU compilation and native differential shards.
Those results remain historical execution evidence, not implementation
acceptance. No source3 commit, ledger76, push or merge is authorized by them.

Per-family frame separation is insufficient. Stop adding individual deep-shape
patches. The resource invariant must compose across every supported ordinary
expression kind and its error path.

## Required invariant

Keep all current parser, construction, query, source and allocation limits,
assertions, workloads and deadlines. Do not increase the process stack, change
compiler instrumentation, narrow SQL admission, add a depth-triggered semantic
fallback or count a native parser rejection as a Modern value-path test.

Ordinary expression-tree depth must not accumulate C++ activations. Binding and
lowering must retain native depth-first order with explicit compiler-owned
continuations. Semantic query/definition-scope reentry remains separate and
must have its existing bounds and restoration proven before implementation.
Pure and mixed tests are both required; neither is a proxy for the other.

## Existing implementation inventory

| Mechanism | Existing owner and contract |
| --- | --- |
| Syntax | Immutable, postorder statement expression arena; independent construction/parser bounds |
| Binding entry | `BindExpression`; query-owner/height, value, syntax-query and ON-origin save/restore |
| Bound publication | One `AppendExpression`, existing property/origin choices and arena checkpoints |
| Function lookup | One decoded name part, registry lookup, native override/arity/modifier order |
| Binding traversal | Recursive typed unary/binary/function/collation/IN children; separate semantic query/template helpers |
| HAVING transfer | Existing explicit frame vector; a different semantic transformation, not a second binder |
| Value emission | One `EmitExpression` entry and typed dispatch, with representative registers authoritative |
| Query-logical values | Existing explicit frame vector and native operand/null/boolean scheduling |
| Conditional branches | Existing task and pending vector; logical/NOT/truth/alias normalization |
| Bytecode publication | Existing builder, homes, constants, symbols, labels and typed errors |

No shared general continuation container exists under `base`. This current
resource requirement needs one shared internal owner, not independently
implemented small stacks in binding and lowering. Use
`include/modern_sqlite/base/compiler_continuation.hpp`, with one active frame in its caller,
one inline pending frame and ordinary vector overflow. It is not a public API
or configurable container. Preserve the existing exact shallow allocation
assertions and all ceilings; no artificial legacy-allocation mode is allowed.

The owner requires nonthrowing frame moves/destruction, never copies frames,
and releases pending frames in explicit LIFO order. A failed overflow push
leaves already retained frames owned; it propagates allocation failure rather
than changing the traversal or silently truncating work.

## Decision

### One continuation chain per canonical compiler phase

Retain the public binder/lowering interfaces and their existing immutable
arenas. Replace recursive ordinary child calls in the canonical path; do not
add a second binder, evaluator or alternative expression walk.

Binding continuations own exactly the existing live data needed after a child:
syntax IDs, scope/phase, one decoded function name when necessary, child IDs
and the existing argument/order vectors. Preserve registered scalar overrides,
likelihood probability validation before its operand, incoming operand-only
binding, wildcard exceptions, argument-before-error order, aggregate ownership,
publication properties and error cleanup.

Lowering generalizes the current value/conditional continuation patterns into
one ordinary emission chain. Preserve whole-expression representative dispatch,
native truth selection and flags, eager/query-logical operand order, conditional
NULL policy, label creation/binding order, instruction/register order, function
and collation symbol timing, list-IN behavior and query lifetime.

Use one current operation and one continuation chain, not one queued task for
every sibling. Argument/list cursors advance in native order. Rich error
objects live at the driver/operation boundary rather than in every retained
continuation. No `std::function`, per-node coroutine allocation, new bytecode,
new evaluation cache or copied syntax/plan arena is introduced.

Retire ordinary recursive implementations when their phases move into the
driver. Existing typed publication and leaf helpers remain canonical; a
second recursive fallback is not part of this design.

A frame borrows stable syntax/plan IDs and strong register/symbol/label IDs,
never a view into a movable continuation or growing bound arena. In particular,
derive a decoded name view from its current owner rather than retaining a
self-reference across a frame move. Do not retain frame references across a
push that can relocate storage.

### Allocation and restoration are part of the design

Shallow expressions must retain existing allocation contracts. An inline first
continuation is the same bounded ownership exception already accepted for the
inline root query; deep overflow uses one ordinary vector per phase, not one
allocation per node, a new registry or a copied expression arena.

Do not impose a new continuation-depth limit from source height alone:
generated singleton-IN/USING/alias structures can expand the bound graph.
Derive and document the actual bound from the owned graph and current semantic
limits. Keep allocation ceilings unchanged and test the overflow allocation
and recovery path deterministically.

Binding entry state needs one explicit root restoration owner. Normal resume
restores the parent node's active property scope before finalization. Failure
and allocation failure restore the caller's entry state without depending on
vector element destruction order. Reentrant semantic helpers must retain their
existing query/template checkpoints and ownership; no silent state defaults.

## Completed responsibility and ordering map

Pinned native entry/child traversal is `walker.c`'s `sqlite3WalkExprNN` and
`sqlite3WalkExprList`. Function and SELECT/IN resolution use `resolve.c`'s
`resolveExprStep`, including its explicit function-argument walk and restored
aggregate permissions. The native value/branch maps in ADR-0104,
0152,0162 and0163 remain authoritative. This changes their C++ activation
ownership, not their algorithms or output contracts.

### Binding phases

| Kind | Entry, child order and completion to preserve |
| --- | --- |
| Cached/normalized syntax | Existing construction binding returns without retagging; parser constants use the existing publisher |
| Literal/variable/name | Existing literal-demand, DQS, parameter/name, alias and typed diagnostic helpers |
| Parentheses | Bind the child and return its ID; no replacement publication |
| Unary | Existing signed-minimum/token shortcut before a child; otherwise bind the operand, then publish with the parent's flags |
| Binary | Existing unsupported-pattern rejection, left then right, canonical comparison/truth/binary finalization |
| COLLATE | Operand first, then dequote/intern the syntax collation and publish |
| Empty IN | Discarded form returns its constructor constant without binding the operand; preserved form uses discarded-value scope |
| Singleton/list IN | Operand, ordered RHS values with the existing retirement-blocked scope, then the existing normalization/affinity/collation publisher |
| Scalar/lazy function | Decode once; argument bound and registered override lookup before special handling; ordered argument vector, modifiers, symbol/collation, publication |
| Likelihood | Same arity/wildcard distinction; resolved probability before operand; incoming operand-only binding and absent annotation; unchanged final modifiers/collation |
| Function errors | Same argument vector and order; release that vector before the final unknown-name/wrong-arity diagnostic; wildcard exception unchanged |
| Aggregate | Checkpoint/capture prefix and argument scope first; ordered arguments, prohibited-scope error, local ordering, FILTER, DISTINCT, then existing dependency/nearest-owner/replay/publication logic |
| Scalar/EXISTS query | Existing scoped query owner/cache, captures, width demand and source restoration; no new query implementation |

`BindExpression` remains the sole entry. Its root restoration owner preserves
query-owner/height, value requirement, construction query flag and ON origin.
Each resumed frame activates its own syntax ID/scope before finalization.
Ordinary child descent does not call `BindExpression` recursively.

Aggregate ordering preserves its own post-child collation requirement and
NULL-order derivation. Aggregate replay/outer ownership is not recomputed by a
new walk: preserve the existing invocation/checkpoint helpers and their
allowed/discarded-order guards.

Definition copies retain the scoped query-cache base and
`binding_validated_template_` ownership. Alias copies disable result-alias
lookup in their definition scope; query-containing copies bind their admitted
query records through the existing root entry. Aggregate replay changes to its
already accepted allowed/replaying mode before reentry. These semantic
boundaries must not be used as ordinary child recursion.

### Preparation and inspection phases

The root's unannotated `ExpressionSyntaxHeight` is itself recursive today.
It must use the same height-fold semantics without ordinary C++ child calls;
postorder normalization keeps using the same fold. Preserve parentheses,
COLLATE's annotated height, unary-plus normalization, singleton/negated-IN
increments, SELECT heights and constructor saturation at1001.

Audit and remove ordinary-depth C++ recursion from the same preparation chain:
the syntax constant walk and its partial TRUE/FALSE rewrites, bound constant
classification, syntax equivalence and bound semantic equivalence. Retain
their distinct pruning/child order, deterministic-function timing, query
terminals, aggregate-invocation comparison and optional likelihood annotation.
They share the continuation owner, not a second semantics implementation.
Do not replace an ordered selective walk with eager whole-arena property
calculation or add another per-expression cache.

Existing bounded source/query dependency and non-null-row proof mechanisms
remain their canonical owners. Distinguish their native pruning rules from
constant/equivalence inspection rather than substituting one proof for another.

### Emission phases

| Kind | Ordered work retained in the one emission chain |
| --- | --- |
| Value entry | Whole-expression representative dispatch first, then the existing query-logical/typed selection |
| Literal/parameter/column/rowid/aggregate | Existing leaf publication, source-frame routing and typed errors |
| Alias/likelihood/COLLATE | Existing value delegation; no conditional wrapper stripping |
| Unary | Operand then original unary instruction; unary plus is value delegation |
| Eager binary | Existing selector; selected descendant then boolean coercion, or left/right homes then original binary instruction |
| Query-logical value | Existing native first operand, skipped/done labels, second operand, binary/coercion and label completion |
| Value comparison/truth test | Same operand order; collation/constants/labels remain at their existing pre/post-child points |
| Scalar call | Function/collation symbols and direct-column shortcut before children; argument homes in order; one existing call instruction |
| Coalesce | Same end label, reached values, not-NULL branches and final label |
| Conditional value | End, each next, false/NULL condition, selected value, jump/bind next, odd default or even NULL, bind end |
| Logical/NOT/truth branch | Same native selection, query-sensitive order, pending sibling/local-label order and NULL disposition |
| Comparison branch | Native nullable query operand order, late NULL/fallthrough labels, original comparison/register branch |
| Membership branch/value | Original collation/descriptor; reusable RHS construction before LHS, or LHS before reached linear RHS; same NULL accounting and result labels |
| Query value | Existing cached/correlated query frame and private read-call lifetime; query bodies remain scoped semantic roots |

The current query-logical frame vector and conditional pending vector become
this same canonical emission chain. Do not retain both old walks beside a new
driver. All ordinary recursive child calls in the typed emitters are replaced
by driver transitions. Aggregate/source/mutation consumers continue to call the
same expression/predicate entry; they do not get another emitter.

### Semantic boundary and resource proof

The canonical reference inventory includes28 binding and48 value-emission
references, including their entry definitions. Statement clauses, source accesses, aggregate
services and mutation destinations are roots; typed ordinary child sites are
the edges removed above. Root/query/definition callbacks retain their existing
lexical owners and cumulative query/height guards.

No ordinary child edge may remain as a reentrant driver call merely to pass
one mixed regression. Closing review checks this call-site inventory against
the implementation. C++ activation depth is then independent of ordinary
tree depth; pending storage follows the actual owned graph, while semantic
query/definition reentry is separately bounded and tested. Keep the existing
43/44 nested-query and construction/normalization boundaries, and add mixed
query/alias/error cases so the proof does not assume all roots are source-free.

The map includes pre-child work, child order/scope, post-child work, pruning,
error phase and failure cleanup for:

- literals, variables, names, aliases and semantic expression copies;
- unary/parentheses/collation, eager arithmetic, comparisons and truth tests;
- scalar/lazy/likelihood functions, overrides and function-error paths;
- aggregate arguments, aggregate ordering/filter and owner/replay contexts;
- empty/singleton/list-IN constructors and all emitted membership strategies;
- scalar/EXISTS query reentry, captures, cached values and shared mutation hosts;
- all callers of binding/value/branch entry, separating ordinary child edges
  from semantically bounded root/query callbacks.

The tables close responsibility, child order, ownership and post-child work.
Concrete frame fields/phases implement this map without enlarging public
bound/physical/program carriers. Exact implementation acceptance and all
verification gates remain required.

## Implementation responsibility map

The pinned preparation/inspection map additionally names
`expr.c:2541-2661` (`exprNodeIsConstant` and its selective walker),
`expr.c:2838-2889` (BINARY-group pruning before constant inspection), and
`expr.c:6668-6777` (ordered expression/list comparison). Keep Modern's
existing syntax-prefilter plus bound-identity comparison, including its
conservative matches, rather than admitting a stronger equivalence proof.

| Canonical owner | Continuation and retained semantics |
| --- | --- |
| Syntax height | Child cursor and maximum; the original height fold remains authoritative |
| Parser-time constants | Entered/child/result state; partial keyword rewrites, NoIs pruning and post-argument function lookup |
| Binding | One `BindingFrame` chain; typed leaf/publication helpers and aggregate checkpoint/replay owners |
| Bound constants and BINARY-group inspection | One selective constant walker with separate value, NoIs and grouped modes; matched group roots prune before child traversal |
| Source/query dependencies | One canonical header-defined ordered driver; terminal-first entry, exact node/alias/capture actions and final-child tails |
| Semantic query occurrence | The canonical `BoundExpressionContainsQuery` walk; left/right/list short circuit and aggregate arguments, FILTER, then ordering, independent of construction flags |
| Syntax equivalence | Ordered expression pairs; function names/modifiers before arguments, each ordering term before its child, then FILTER |
| Bound/invocation equivalence | One pair chain for ordinary expressions and aggregate invocations; aliases, source identity, optional likelihood annotation and ordered attachments retain their original rules |
| Literal defaults | Parentheses/positive tails and pending negative operators; signed-minimum shortcut, numeric promotion and typed unsupported leaves are unchanged |
| Result references and truth inspection | Iterative ordinal/wrapper traversal and post-child COLLATE publication; no additional binding entry |
| Joined output inspection | The original column/alias/coalesce-only predicate, with ordered continuations rather than broader expression eligibility |
| Outer aggregate query reparenting | The same ordinary child cursor as constant inspection; SELECT remains a terminal lexical-parent update |
| Logical selection | Pure RIGHT-then-LEFT peeking with its own inspection continuation; only genuine aliases are transparent, and an unselected child retains its original ID |
| Value/branch/membership emission | One `EmissionFrame` chain and operation driver; the superseded recursive emitters and pending task/vector implementations are removed |

Logical peeking emits nothing and adds no expression cache. It retains the
same native truth observer and declared/effective ON ownership; it does not
reenter an emission driver. Wrapper inspection likewise retains the
unannotated likelihood barrier instead of treating every function as an alias.

The completed binding inventory has17 textual `BindExpression` references,
including its definition: mutation values/predicates, ON/HAVING/WHERE, VALUES
and incoming construction roots, result columns, grouping/ordering probes,
LIMIT/OFFSET and scoped definition copies. The value-emission inventory
has24 `EmitExpression` references, including its definition: projection and
aggregate services, source keys/ranges, mutation destinations, ordering
payloads, and VALUES/compound/DISTINCT consumers. None is an ordinary child
edge. Typed query leaves and definition/replay helpers retain their semantic
root ownership; lexical source/query-frame lookup is not expression descent.

`BindExpression` restores its root property/height state. Nested query and
definition-copy owners restore name scopes, active query scratch, cache base,
validated-template mode and expression ownership on both typed failure and
allocation unwinding. Replay modes use the existing scoped-mode mechanism.
The lowerer's aggregate expression/predicate and completion scopes share one
restoration owner for representative source and grouped-output mode, including
allocation unwinding. Pending frames never own these caller entry states.

### Bound from owned graphs, not another admission limit

Let `S` be the owned syntax-expression count and `B` the owned bound-expression
count. Ordinary syntax children are postorder descendants. Bound publication
creates parents after their children; genuine alias targets already exist.
Query destinations and aggregate contexts are separate semantic roots, not
ordinary child cycles. Generated singleton-IN, USING and alias structures
therefore use their actual owned graph, not an assumed source-height budget.

Height, binding, constant, dependency, wrapper, joined-output and reparenting continuations
are bounded by their active path in `S` or `B`. A paired equivalence path
descends at least one of its two graph positions and retains at most the sum
of those path lengths, plus its optional invocation root. Emission has only
value, branch and membership requests: same-ID adapters advance through these
modes, never back to an earlier mode; their children descend the bound graph.
The conservative pending-storage bound is three operation frames per bound
graph position. These are storage proofs, not new runtime depth limits.
The shared owner still keeps one active caller frame, one inline pending
frame and vector overflow with explicit LIFO cleanup.

Semantic query/definition reentry retains cumulative native height checks.
The500/480 outer-aggregate/query shape is rejected by both engines for
cumulative height and is not an admitted regression. Its240/240 counterpart
is admitted and verifies the reparented query owner. Deep query-alias copies,
their scalar-width error, the existing43/44 query boundaries,500-deep signed
defaults/ordinals/COLLATE references, and pure/mixed ordinary paths remain
separate controls.

Two additional genuine inspection reds at tree `d0d19f2d` produce the native
Windows resource status `0xc00000fd`: ordinary ordering equivalence and
aggregate invocation equivalence. All eight corresponding pinned-native
preparations succeed. Constant-membership and grouped-constant controls are
already green before their behavior-preserving conversion. The binder's
all-prefix allocation scenarios grow from63 to69, and lowering's related
scenarios from10 to14; original1024/256 ceilings and every existing exact
allocation assertion remain unchanged.

## Shared dependency-walk refinement (accepted)

The public typed-row control rejects tree `151d899a`, despite its2132 isolated
units, complete2169-name native correctness/tooling union,25 genuine GNU
compiler obligations and nine native quality owners. Those remain historical
execution, not implementation acceptance. The new owning
`ReadSession.PreservesMixedOrderedAggregateDepthOnTheDefaultStack` fails at
test tree `17c3c86e`; native returns two identical group-concat values for
parameter0/2 and two NULLs for parameter NULL.

The admitted SQL repeats `group_concat(argument ORDER BY argument)` twice.
Its argument has500 `abs` wrappers around `?1` plus480 `+0` operations.
Direct parse/bind/plan/lower/VM construction succeeds, but public preparation
fails with `0xc00000fd`. An actual symbolic debugger trace contains9346 frames:
`ReadSession::Prepare` -> shared compilation -> binding ->
`FinishAggregateCall` -> the recursive canonical `VisitQueryDependencies`
scalar/binary traversal. Five successful probes of another visitor
instantiation did not prove this instantiated path's resource bound.

Do not shrink another caller frame or special-case ordered aggregates.
Convert that existing canonical header-defined dependency walk; do not add
a second dependency/proof mechanism. Every binding/value/node-visitor consumer
uses the same implementation. Preserve:

- node validation and the node visitor's Continue/Prune/Abort action before
  identity, context or child observation;
- aggregate context and SELECT-capture terminals, including binding/value
  source-capture filtering and immediate visitor abort;
- genuine alias context before its target, ordinary left/right order, then
  ordered function/IN list children; no SELECT-body traversal;
- original empty-list success, malformed-ID failure and borrowed lifetimes.

The one shared continuation owner must also be reachable from this public
header-defined implementation. Move it, without a duplicate or forwarding
copy, from `src/base/compiler_continuation.hpp` to
`include/modern_sqlite/base/compiler_continuation.hpp`. Its
`compiler_detail` namespace and internal contract remain unchanged; this is
an implementation dependency, not an admitted public container API.
Update all actual includes and ownership tests. Do not introduce type erasure,
function wrappers, a dependency cache or another container to avoid the move.

Move the existing ordinary-child cursor into `binder_detail` beside the
canonical walker and reuse it from binder constant/reparenting inspection.
It only enumerates ordinary IDs; each native proof retains its own entry,
pruning and terminal rules.

The dependency driver owns one active cursor and the same inline-first stack.
Process an entered terminal child before deciding whether its parent needs a
continuation. Retain a parent only for a nonterminal child with remaining
siblings; a final child is a tail transition. This preserves one ordered visit
per node and avoids adding storage allocations to ordinary shallow comparisons
merely to return from a terminal callback. Overflow remains explicit and all
existing exact allocation assertions/ceilings remain unchanged.

Qualify the genuine public red, direct ordered callback/prune/abort/invalid-ID
controls, existing capture and ownership cases, every affected allocation/OOM
owner, complete units and typed native rows/reset. Public read and shared
mutation hosts remain consumers of this same walk. No commit, ledger76 or
source acceptance follows until this last ordinary dependency edge is removed
and the complete current source is independently accepted.

### Bounded overflow growth (accepted)

The canonical dependency conversion makes the genuine ordered-aggregate red
green and matches all28 public SQL controls across84 bind/row/reset observations.
Its142-case owning selection retains every existing exact allocation assertion,
but the unchanged optimizer OOM sweep rejects the256-OR proof fixture:
152 allocations exceed its original128 ceiling. This is a real allocation
regression, not permission to increase the ceiling, remove the proof workload,
add a recursive fallback or add a source-specific shortcut.

The installed native vector's measured capacity progression for255 pending
overflow frames is
`1,2,3,4,6,9,13,19,28,42,63,94,141,211,316`:15 allocations.
Requesting geometric doubling uses
`1,2,4,8,16,32,64,128,256`:9 allocations for the same255 frames.
This identifies standard-library-dependent overflow growth, not SQL evaluation
or a persistent-format mechanism.

Keep the same single internal owner and ordinary `std::vector<Frame>`. Before
an overflow push into full capacity, reserve one slot from zero or request
twice the current capacity. Saturate at `max_size()` without arithmetic
overflow; an exhausted vector keeps its ordinary allocation/length failure.
Do not add a reserve hint, configurable capacity, cache, extra inline slots
or another container. The first inline pending frame still allocates nothing.
Moves/destruction remain nonthrowing, a failed reserve retains all pending
ownership, and pops/LIFO cleanup allocate nothing.

This makes the requested overflow growth geometric on every supported
standard library while remaining bounded by the active owned-graph path.
Existing exact assertions and1024/256/128 ceilings remain unchanged. Add a
focused256-frame growth-budget red on native Windows before implementation,
then require its green, every overflow-failure prefix/recovery, all original
allocation/OOM owners, complete units and the matched public controls.

The accepted growth implementation makes the142-case owning selection green,
including the original48-by-two optimizer OOM sweep and every exact allocation
assertion. The public matrix additionally covers source-free/scanned/global
aggregate guards, grouped ordering equivalence, DISTINCT ordering and logical
scan guards:34 SQL controls and102 matched typed bind/row/reset observations.
These targeted results still do not constitute complete-source acceptance.

### Public semantic query-inspection closure

Independent source review rejects tree `c8c5e502` because the unannotated
public `BoundExpressionContainsQuery` implementation still descends ordinary
children recursively. Scan UPDATE/DELETE planning calls it even for a
query-free predicate when the table has no secondary indexes. This is not
semantic query-body reentry and must use the already accepted shared owner.

The owning direct-inspection, scan-mutation and aggregate short-circuit
controls at tree `3fa10c0a` are green on the old implementation; all four
pinned-native deep UPDATE/DELETE preparations also succeed. No executed
overflow is claimed for this control. This is a behavior-preserving conversion
from an actual green baseline, closing the required no-ordinary-recursion
invariant rather than manufacturing a failing semantic test.

Retain this distinct semantic occurrence predicate, not construction flags or
the dependency walk's aggregate-context terminal. Use the same raw ordinary
child cursor and `ContinuationStack`; its aggregate extension visits
arguments, then FILTER, then ordering. A subquery is an immediate true terminal,
and an earlier true child must prevent observation of later children.
Entered false terminals do not retain a parent; a final child is a tail
transition. Existing valid-ID/invocation assertions, alias traversal,
short-circuit order, allocation failure and borrowed arena ownership remain.
Deep query controls deliberately clear the construction query flag with
COLLATE while retaining semantic containment and rowid-collection decisions.

## Verification and delivery

Add separate binder, lowerer and public-session reds for the reproduced mixed
controls before implementation. Qualify construction/parser boundaries and
mixed near-boundary cross-kind cases, not just another fixed wrapper chain.
Require native rows/types/errors, immutable trees, bytecode/control ordering,
callback work, all affected allocation/OOM/lifecycle owners and complete fast
units under the original limits.

Continue the current uncommitted third source slice under ADR-0163/0164:
the recursive selector/value boundary is not acceptable until this required
compiler-resource contract is complete. Keep the two published accepted
commits unchanged. If the integrated source cannot be reviewed as one coherent
slice, accept an ADR-0106 delivery split before committing; no half-qualified
resource implementation or known failing suffix may be sealed. General
literal/name/unary/static-width and IN-query admissions, node43 and later nodes
remain incomplete.
