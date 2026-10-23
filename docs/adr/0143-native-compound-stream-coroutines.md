# ADR-0143: Native Compound-Stream Coroutine Prerequisite

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Confirmed root mismatch

PR #245 closes resident comparison, not compound execution scheduling.
The pinned source does not implement ordinary UNION/EXCEPT/INTERSECT through
the unordered membership strategy selected by ADR-0056.

`select.c:3070-3089` invents an ORDER BY when needed and chooses merge for
every non-ALL compound except the recognized UNION LIMIT-one shortcut.
`select.c:3399-3815` runs resumable child streams. Materializing complete
children before returning a row is not equivalent when LIMIT prevents later
expression evaluation.

The existing public trace runner confirms this ordinary query returns an
overflow error before any row in Modern, while pinned SQLite returns `1,2`
and completes:

```sql
SELECT id FROM items
WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1)
UNION SELECT id FROM items WHERE id<=3
LIMIT 2
```

The frozen read-only matrix also reproduces explicit ORDER BY, INTERSECT and
EXCEPT mismatches. UNION ALL, recognized UNION LIMIT-one and LIMIT-zero
controls remain correct. OFFSET has its own native lookahead/error boundary:
do not replace the actual schedule with a guessed earlier limit check.

Stop incidental temporary-key adapter optimization until this root has a
canonical stream solution. Existing EXCEPT/INTERSECT timing controls compare
the same SQL but not the same native algorithm.

## Native and canonical mechanisms

| Responsibility | Pinned SQLite / current Modern mechanism |
|---|---|
| Initialize, exchange resume PC, end at caller's exhaustion edge | `vdbe.c:1221-1294` InitCoroutine/Yield/EndCoroutine |
| Merge, lookahead, duplicate/representative and LIMIT schedule | `select.c:3204-3214,3399-3815` |
| Existing private region identity, call graph and summaries | ADR-0082/0083; `AnalyzeSubroutineGraph` and canonical transfer |
| Existing exact capability ownership | `InstructionOwnership` operand-check callbacks and capability lattice |
| Existing cleanup | VM `ClearExecution`, cursor/temporary/aggregate owners |
| Existing query producers | `StreamingCorePlan`, `SelectOutputContext`, positioned source emission |

The old read-subroutine contract requires closed resources on return.
Suspension is a different lifecycle, not permission to weaken those returns
or share a cursor ID between regions.

## Decision

### 1. Add bounded typed coroutine control to the same VM

Add strong coroutine IDs, immutable entry/end/output-range descriptors and
initialize, resume-with-exhaustion-target, yield, end and cancel instructions.
Keep program counters in typed control metadata, not SQL values.
This is an explicit translation of native PC exchange, not C++ `co_await`,
another executor, an embedded compiler or a callback/plugin system.

Coroutine descriptors are distinct from closed-return subroutines. Preserve
the existing descriptor contract rather than adding dormant fields to every
old descriptor. Normalize both through one canonical lexical-region map,
operand ownership visitor, call/effect graph, checked analysis accounting and
capability transfer; do not add a parallel verifier.

Allocate bounded runtime resume/caller state during VM creation. The runtime
tracks uninitialized, running, suspended, ended and cancelled states.
Resume of an ended stream takes its exhaustion edge. Initialization of a
live suspended stream requires prior cancellation. Reject reentrant or
recursive activation; preserve the existing VM Running-phase protections.

### 2. Verify suspension without exposing private mutable state

Each coroutine has one lexical caller owner and a declared output range.
Reuse canonical read/write summaries, including transitive ordinary calls,
to derive its may-written register set. Outside regions may read yielded
outputs, not private scratch, and cannot write coroutine-private/output
registers. Shared captured inputs retain the ordinary initialized-value
contract at initialization and resumption.

Analyze producer bodies through the existing lattice: yield retains local
capabilities and continues at its next instruction for body analysis; every
yield defines the complete declared output range; end requires closed local
resources and stopped child coroutines. Empty end-only streams are valid.
Summaries distinguish yielding and exhaustion edges at the caller.
The caller lattice tracks sets of not-initialized, live and ended states:
merge takes their union, resume rejects any not-initialized alternative,
normal yield retains live and exhaustion defines ended. An end-only body
has no yielding edge. Initialization rejects a possibly live instance;
cancellation removes live state without implying new outputs. These are
analysis states, not another runtime execution mechanism.

Keep ordinary value/read subroutine effects, closed-return checks and budgets
unchanged. Read subroutines that own coroutines must finish or cancel them
before normal return. Reject cross-owner control and recursive ownership.

### 3. Use canonical cancellation and lifetime

Retain the existing resource ownership map only where coroutine cancellation
needs it, and charge all derived maps/summaries to current limits.
Factor the canonical cleanup primitives so whole-execution cleanup and
lexical cancellation close the same cursor, sorter, top-N, relation and
aggregate owners. Cancellation includes the owned child-coroutine subtree,
not siblings or caller resources. No second resource-reference visitor or
handle registry is introduced.

Compile depth-first subtree intervals into the same retained ownership
allocation. Reuse the graph's iterative traversal scratch, charge interval
construction and cancellation visits to the existing analysis-work limit,
and make each containment query constant-time. One cancellation is linear
in the declared coroutine and resource slots, not in their nesting depth.
Keep the existing shallow value-only owned-byte budget unchanged.

Yield does not publish a client row or release its producer resources.
Yielded values remain valid until resumption/cancellation under current
register backing rules. Error, interruption, reset, detach, destruction and
foreign-exception paths retain whole-execution cleanup. End does not repair
malformed open-resource bodies at runtime.

### 4. Repair existing compound consumers, not add SQL scope

This prerequisite supersedes ADR-0056's coroutine deferral only where needed
to repair already implemented compounds. It does not admit FROM-derived
queries, IN, CTEs, views, windows or later DAG nodes.

The consumer correction must reuse existing source/projection/predicate and
ordering producers with a coroutine row destination. Use input order when
the canonical planner proves it, otherwise the existing sorter; do not keep
a blocking-arm fallback for a shape requiring native laziness.
Implicit non-ALL merge order, representative rules, lookahead and global
LIMIT/OFFSET follow the completed native map. That consumer receives its
own completed design and evidence before implementation.

## Ordered delivery

First deliver two reviewed, buildable control sources: value-only coroutine
verification/execution, then read capabilities/ownership/cancellation.
The initial prefix admits main-owned value coroutines without private calls;
nested owners, callable children and read capabilities remain rejected until
the ownership successor qualifies them through the same canonical machinery.
Each has focused red/green, module normal/boundary/malformed/OOM/lifecycle
coverage, fresh full development and complete fast units, GPT-5.6 Sol review,
normal commit and append-only evidence before its successor.

The subsequent canonical producer/compound integration is a separate ordered
delivery, not an unfinished SQL fix hidden in the control prefix.
The confirmed public mismatch remains explicitly open until its native
row/error trace is matched. Preserve positive controls and the observed
OFFSET boundary; do not regenerate existing oracles to accept eager errors.

Use change-aware exact-head closing plans, original models and allocation
contracts, actual native/generated-input/hosted gates and integrated review
after complete ledger publication. Keep strict C++23, instruction/value
layouts, assertions, 2,000-byte negative control, 36/37 budgets and deadlines.
No node or audit-ledger completion is implied by a control-only prefix.
