# ADR-0159: Native VALUES Construction Phases and Producer Chunks

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: shared VALUES correction before the IN query-RHS admission

## Context and completed native mapping

Start from PR #263's accepted main `976983d`, tree
`4dc474534a87c7cb1f7f07790126bbbafc0b8950`. Its scalar membership hash does
not admit query membership. Source research for that admission confirmed
existing VALUES differences through matched pinned-native and Modern public
read traces; these are not speculative audit findings.

The immutable parser preserves one `ValuesCore` and its source rows. The
binder currently normalizes native construction facts only when IN exists,
assigns all parameters before that normalization, binds VALUES in the normal
name/value phase, and chooses one producer-eager/consumer-filtered schedule
for an entire VALUES core after binding. These phases cannot reproduce the
native mechanism below.

| Responsibility | SQLite 3.54.0 source |
| --- | --- |
| First row and subsequent-row grammar actions | `parse.y:675-693` |
| Native SELECT construction identity | `select.c:126-170` |
| Normal constant/no-affinity check | `insert.c:609-620` |
| Ordered coroutine/fallback gates | `insert.c:673-710` |
| Coroutine creation, first-row preparation, row-width check and later-row code | `insert.c:711-789` |
| Normal versus no-IS constant walks, function selection and partial boolean rewrites | `expr.c:2490-2698` |
| Arity-aware native function selection and special-function declarations | `callback.c:279-332`; `func.c:3399-3401,3450,3547-3549` |
| First-row resolution versus incoming inline expression generation | `insert.c:752-780`; `resolve.c:1178-1200`; `expr.c:4670-4740` |
| Constructor-propagated explicit collation and unresolved function lookup | `expr.c:248-311,860-910` |
| Coroutine values versus EXISTS consumer projection elision and completion | `insert.c:746-780`; `select.c:1256-1269,1467-1471` |
| Existing Modern constructor translation and value consumers | ADR-0056, ADR-0152, ADR-0157 |

Native evaluates the new row with the no-IS constant walk first. If that
passes and the current form is not already coroutine-backed, it tests the
current row with the normal constant walk and then checks affinity. A
permitted transition creates a coroutine chunk beginning with that current
row. A failed gate appends a UNION ALL row. After fallback, a later constant
pair may start another coroutine chunk; an all-rows-constant summary loses
this state machine.

Native also prepares constant row code during those grammar actions, before
outer name resolution or a later expression can delete the query. The pure
Modern parser must not copy that binding/VM coupling, but its shared binder
construction translation must preserve the observable phases.

### Executed non-interchangeable controls

- With an outer column TRUE=0, `(VALUES(TRUE))` returns0, whereas
  `(VALUES(TRUE),(2))` returns1 after the native constructor walk.
- A nonconstant second row prevents the first-row walk and retains0.
  A raw IS/IS NOT node stops the no-IS walk before its children, while a
  constructor-folded literal can pass; earlier partial rewrites survive.
- Constant-row collation errors precede an outer missing name or a later
  invalid parameter, survive outer empty-IN deletion and overwritten UPDATE
  values, and do not become ordinary suppressible value diagnostics.
  An earlier invalid parameter still precedes a later constructor error.
- A coroutine row-width error survives deletion, while an unused UNION
  fallback with a name/width problem may be discarded. Preparation of the
  first coroutine row precedes the new row's width check.
- Mixed coroutine/UNION chunks evaluate skipped producer rows before
  consumer OFFSET. Modern currently skips their errors after treating the
  whole core as consumer-filtered.
- A single VALUES row is consumer-filtered. Modern currently evaluates its
  skipped constant error after treating it as producer-eager.
- EXISTS omits a singleton/fallback result expression, but still evaluates
  reached coroutine producer rows. Its first-row completion must not evaluate
  later coroutine rows.
- Invalid special-function arity declines the constant gate without an early
  binding diagnostic; outer empty-IN can discard the resulting fallback.
- The first coroutine row undergoes SELECT resolution. Subsequent incoming
  rows undergo direct expression generation and their syntax is deleted.
  Incoming `likelihood(1,2)` therefore returns1 and ignores its probability
  argument; scalar local ORDER BY is likewise ignored there. The same
  first-row forms retain ordinary probability and scalar-modifier errors.
- Ignoring an incoming probability's value does not erase its constructor
  collation flag. An explicit NOCASE there still affects an enclosing
  comparison, and a required missing collation still fails. An oversized
  hexadecimal probability is ignored without parsing its value.

The source-attested controls and actual Modern transcripts retain typed
rows, preparation/step errors, ordering and paired nonconstant controls.
No timing or whole-engine parity claim follows from them.

## Decision

### 1. One shared construction phase, not another parser or validator

Retain the source-preserving immutable parser. Extend the existing
`SyntaxNormalization` mechanism in `StatementBinder`; do not bind names,
resolve the function registry, mutate syntax or generate bytecode in the
parser.

For statements containing multirow VALUES, schedule parameter assignment,
existing expression-construction checks and completed VALUES-row events in
native source/construction order. Event records borrow validated syntax IDs
and row descriptors, own no SQL values and contain no VM state. One bounded
event schedule transposes the existing immutable arenas; do not repeatedly
scan every query for every expression.

Reuse/extract the current canonical parameter-assignment operation and
expression-construction operation. Preserve source-order named/numbered
parameters, partial constructor progress, argument/depth errors and
diagnostic precedence. Statements without multirow VALUES retain their
current ordinary allocation path and the existing IN normalization behavior.
No second parameter map, boolean walker or bound-expression arena is added.

Structural validation remains before semantic construction. Ordinary
compound/value validation cannot overtake a native constructor error.
Malformed references never become construction work.

Independent design review accepted this shared phase/owner/chunk contract
before production implementation.

### 2. Reuse the constant kernel with an explicit no-IS policy

Add a normal/no-IS policy to the current parser-time constant walk.
Check an already normalized constructor literal first. Otherwise, no-IS
rejects raw IS/IS NOT before visiting their operands. Preserve function
selection, filtered-aggregate pruning, left-to-right short circuiting and
already-performed TRUE/FALSE rewrites.

Track each VALUES core's current UNION/coroutine form and current row.
Run the new-row walk before the current-row walk, exactly as mapped above;
do not eagerly normalize all boolean names or replace the state with
post-binding determinism. Function lookup is arity-aware after the argument
walk. Extract the existing special-function arity policy from ordinary
binding and reuse it in both paths; an invalid arity is not a constructor
binding error unless native actually reaches binding.
The currently admitted pre-resolution constant
grammar has no CAST/expression affinity constructor. Preserve that audited
boundary rather than guessing affinity for future syntax; a later admission
must extend this same mechanism.

WITH, schema initialization and special parses are not silently treated as
ordinary VALUES construction. They require their actual context when those
surfaces are admitted.

### 3. Prepare native constant roots once through the canonical binder

When native creates constant row code, use the existing binder expression
and value-requirement kernel to perform the same preparation. Such roots
have no free source captures, aggregate owner or result-alias lookup.
Prepare the current first row before comparing the incoming row's width,
then prepare the incoming row.

Distinguish those two native preparation contexts in the same binder kernel.
The first row retains ordinary SELECT expression resolution. A narrowly
scoped incoming-row preparation phase models native expression generation,
not another name-resolution pass or actual binder-side bytecode emission.
It ignores scalar local ORDER BY, and lowers built-in likelihood wrappers
through the existing value-neutral alias expression without value-binding the
unused probability. The constant gate has already checked function arity,
all argument constants and FILTER/window exclusion. Registered scalar
overrides retain their ordinary function implementation.
Restore the preparation phase on every normal, typed-error and allocation
exit. Do not apply it to singleton/fallback rows, first coroutine rows or
ordinary expressions. Incoming syntax discarded by native never acquires
later resolution-only checks through the cache.

Preserve the unresolved function's native explicit-collation provenance from
all arguments, including an unused probability. Extend the same postorder
`SyntaxNormalization` annotation with a borrowed collation-name span, retaining
native constant/deletion boundaries and left-to-right precedence. Reuse
canonical collation-name interning when publishing bound properties; do not
validate an unused name unless a value consumer requires that collation.
Ordinary bound-property propagation is insufficient here because binding the
unused value would incorrectly parse an oversized hexadecimal literal or
require an ignored function's collation. Do not bind a dummy value, add a
second syntax traversal/container, or introduce a probability default.

Retain the resulting canonical bound-expression identity through the same
syntax annotation and reuse it during ordinary binding. Successful roots
must not be bound/replayed twice. Do not add a parallel collation/function
validator, use a second expression cache or defer the error until row
emission. Preserve that identity over arena checkpoints, moves and immutable
query/template copies.

Construction diagnostics precede later name-only/overwritten-value policy.
Later binding must not drop them or publish success. Parameter occurrences
are assigned at their actual source points, not globally in advance of an
earlier constructor diagnostic.

### 4. Publish native producer chunks through the existing VALUES owner

Keep the current producer-eager and consumer-filtered row emitters. Record
the constructor's chunk boundaries and schedules in the existing bound
VALUES owner; uniform cores retain the compact existing schedule.
Mixed cores publish ordered, non-overlapping chunks covering every row
exactly once. A new coroutine chunk may include the immediately preceding
row; retain that transition rather than marking only newly parsed rows.

Single-row and UNION fallback rows are consumer-filtered. Coroutine chunks
are producer-eager. Feed those facts through the existing logical/physical
views and row producer, selecting the existing offset/value order per chunk.
Producer-eager values use the canonical expression emitter even when an
EXISTS consumer elides its projection; consumer-filtered values retain that
elision. Keep the existing LIMIT completion before subsequent producer rows.
Do not create a second VALUES executor, sorter, query owner or temporary
container. Do not expose a fabricated root SELECT.

Preserve result metadata, scalar LIMIT/OFFSET, compound/coroutine completion,
correlation captures and read/write ownership. The same constructor and
producer contracts serve scalar/EXISTS and mutation hosts.

## TDD, ownership and close

Start with the exact public TRUE-name row mismatch and record actual red
before production changes. Add the paired singleton/nonconstant/no-IS and
partial-progress cases, early collation versus name/parameter/deletion/
overwritten-value controls, first-row/width ordering and mixed/single-row
OFFSET errors. Tests cover read and write consumers, metadata/captures,
malformed input, owning allocation failure, reset/rebind and rollback.
Cover invalid arity in every special-function family, the first/incoming
resolution distinction, ignored versus required arguments, and maximum-depth
normal/no-IS walks without changing the expression or process-stack limits.

Keep all declared limits, allocation ceilings, workloads, assertions and
deadlines. Bound construction-event counts by validated syntax/input limits
with checked arithmetic and typed OOM. No unbounded secondary registry or
success-shaped fallback is allowed.

Run focused green, complete fast units, the actual change-aware plan,
necessary native/differential and isolated compiler/sanitizer selections.
Review the completed source mapping and implementation before publication;
reviewers do not discover the oracle or run routine qualification.
ADR-0053 continues to defer new timing families and optimization.

No IN query RHS, row values, CTE, view, constraint or later DAG node is
admitted here. Query construction identity, immutable RHS sharing, native
stored membership and Bloom/VM integration remain subsequent owning work.

## Closing amendment: bounded read-session preparation fault domains

The final-head sanitizer run `40443687838` passed the production regressions
but timed out in `session.read_session_oom.1` at10.03 seconds, after partition0
took9.90 seconds. Accepted PR #263's same four cases took9.22/9.12/9.07/9.00
seconds. Both jobs used GNU13.3 on the same Ubuntu24.04 image family; this
does not establish an engine-algorithm regression or justify changing hosts.
The existing ten-second case limit and ASan serial reservation remain.

Parent-owned phase measurement rebuilt the exact current ASan OOM target,
byte-verified its owned executable copy on the native filesystem, and ran
the original partition1 diagnostically. It completed in11.31 seconds;
9.85 seconds were the ten consecutive preparation-query sweeps. Their
baseline allocation counts were341,282,351,420,412,345,307,349,361,368:
3,536 failure boundaries across the existing four partitions. Startup and
the remaining lifecycle/step/reprepare domains did not dominate. This direct
diagnostic is not a passing ten-second qualification result.

Keep the same executable, immutable input bytes, owned native temporary
files, allocator, ten SQL strings, baseline count limits, fresh sessions,
injected operations, lock checks and successful retries. Extract the existing
preparation sweep once and register each SQL/partition pair as its own named
isolated CTest case. Reuse the current four-way allocation partition and
canonical ASan serial property list; do not add another runner or scheduler.
All40 named preparation cases retain the original ten-second deadline.

The four existing combined cases retain the other lifecycle, overflow,
step, reset and schema-reprepare domains. Their preparation domains move to
the40 named cases; they are not sampled or omitted. The unpartitioned
diagnostic entry point still runs the complete original population through
the same extracted helper. Add a strict named preparation selector and its
unknown-selector control, following the current temporary-consumer pattern.
Keep resource locks, ordinary/sanitized process isolation, every other
registration, CI command and job envelope.

Before acceptance, mechanically compare all ten SQL strings and every
baseline count, then expand each old and new `(SQL, failure-index)` domain
and require exact multiset equality. Keep every OOM, released-lock and retry
assertion in the extracted helper. Execute all40 named cases and all four
remaining combined cases under their real original limits, including ASan,
then complete the fast/native and final-head hosted selections. No production
algorithm, format, stack, sanitizer option or value behavior changes for this
verification-granularity correction.
