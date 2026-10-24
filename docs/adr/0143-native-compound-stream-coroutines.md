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

## Compound integration amendment

Status: Accepted. This amendment repairs existing compound execution after
PR #249; it adds no SQL surface or query owner.

The unchanged merged baseline still differs on UNION/EXCEPT/INTERSECT early
output, OFFSET lookahead and a LIMIT-one UNION containing an EXCEPT prefix.
Additional native controls distinguish local duplicate collations from
inherited merge collations, a late unused COLLATE, complete-key permutation,
initial exhaustion and the outer explicit-COLLATE wrapper.

### Complete native boundary

| Responsibility | Pinned source / canonical Modern boundary |
|---|---|
| Local result collations and inherited merge keys | `select.c:2632-2683`; bound expression properties and existing comparison owner |
| Concatenation, implicit order and syntactic UNION LIMIT-one | `select.c:2990-3148`; existing compound nodes and integer-literal classifier |
| Duplicate suppression, OFFSET, destination and LIMIT | `select.c:3182-3392`; existing row-destination emission |
| Complete keys, balanced homogeneous splits and input bounds | `select.c:3520-3681`; physical compound edges and per-core order requests |
| Initial A/B acquisition, equal-key actions and exhaustion | `select.c:3683-3793`; existing typed coroutine control |
| Explicit-COLLATE wrapper without the inner LIMIT | `select.c:5591-5670`; existing set-then-order semantic boundary |
| Value ownership across yielded rows | `select.c:3307-3318`, `vdbe.c:1661-1696`; existing register-range move |
| Reached-site outer column reads | `expr.c:4522-4568,5121-5207`; existing source-frame field/rowid emission |
| Cache construction and suspension lifetime | Existing statement query-helper bindings, coroutine ownership and source register layout |

### Coroutine data and construction

Extend the existing resume instruction with an optional caller-owned
destination range. On a successful yield, move the declared output range
there without allocation and leave producer slots initialized NULL. An
exhaustion edge defines no destination row. Without a destination, preserve
existing borrowed-output behavior.

Resolve this handoff through the existing continuation and range-move
mechanism. Do not add another continuation frame or permit a caller to write
producer output/private registers directly. Validate destination bounds,
nonoverlap, ownership, initialized outputs and both resume edges through the
canonical transfer analysis.

Admit read-only lexical captures through the same call/resume input summaries.
A reached `ReadField`, `ReadRowId` or direct column-function read may borrow
an ancestor-positioned cursor; transitive callers must prove that position on
each call/resume. Opening, positioning, invalidation and closing still have one
owner. Borrowed uses neither claim that cursor nor enter child cancellation.
Apply the analogous read-only ancestor rule to captured representative and
aggregate registers; unrelated/sibling private access and outside writes stay
invalid.

Keep actual reads at their conditional sites through the existing live source
frame. Do not eagerly load captures, copy cursors, or add a runtime reader.
Preserve skipped-field failure behavior and require borrowed inputs to remain
valid when resuming; existing owner-return and cancellation rules prevent a
producer from outliving its source.

Allow only main-prologue constant construction of non-output coroutine-private
slots before any private call or coroutine activation. This narrow lifetime
exception keeps existing statement-wide query caches valid across suspension,
cancellation and reinvocation without another cache, reset policy or once
executor. Reject mixed control-flow alternatives that may already have
activated private code, nonconstant writes, outside private reads and every
outside output write. Charge the proof state to existing analysis limits.

### Producer and comparison ownership

Keep one `PlanLowerer` per bound query. Group existing read-register slots in
one canonical layout and borrow the active lexical producer's layout while
emitting. Producers that can suspend concurrently have disjoint scratch and
ordering capabilities; sequential sources in one region retain safe pooling.
Query-ID-indexed expression, function and aggregate mappings remain unique.

Generalize the existing row sink for coroutine output, reusing the same
source, aggregate, DISTINCT, sorter/top-N and prefix-block emitters. Coroutine
output is a complete result-row range, not an encoded record or a second
executor. Preserve copied input bounds only where native propagates them.

A single advanced ordered core uses the same producer workspace and final
row destination without a coroutine. Retire the old multi-arm sorter and
materialized-set strategies, retaining ordinary DISTINCT membership.
Cold-lowering checks retain their numerical ceilings and repeated-run
stability; removing obsolete owners may reduce historical allocation counts.

Give the existing register-comparison owner explicit field-index metadata,
including repeated/permuted keys, using the same prepared record comparator.
Replace its prefix-only metadata contract directly; add no compatibility
adapter or second comparison algorithm.

Publish native balanced edges through the existing physical compound nodes.
Keep inherited merge keys separate from local duplicate collations. Reuse one
bound-property collation resolver rather than applying the whole-query
collation vector to every subrange. Move existing per-core physical ordering
metadata to its query owner so implicit ordering needs no synthetic bound
query or invented top-level SQL ORDER BY.

Feed each core's effective order and fixed-limit context to the canonical
solver. Native merge removes the child LIMIT expression even when it passes
a runtime bound; do not turn that bound into a syntactic fixed-limit hint.
Keep root OFFSET/LIMIT, wrapper sorting and child bounds in their proper
owners. Remove replaced eager compound algorithms, not preserve them as a
fallback that changes the frozen error schedule.

### Four-source implementation batch

Deliver one bounded batch: coroutine handoff/capture/construction contracts; lexical
read-register ownership as a green refactor; field-index comparison metadata;
then native planning and compound producer/merge integration. Every source
retains its focused/fast/ownership/failure evidence and review before commit.
Do not admit new physical strategies before the final consumer exists.

The final batch must match the frozen row/error/collation/lookahead controls,
including nested calls, captures, VALUES, aggregates, balanced/mixed operators,
reset, cancellation and failure cleanup. Preserve existing allocation caps,
analysis accounting, workloads and deadlines. Run the actual selected closing
union on the final exact head and publish its complete evidence before
integrated review and merge. No further prerequisite is added silently.
