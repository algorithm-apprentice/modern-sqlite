# ADR-0107: Joined Predicate Access Phases

- Status: Accepted
- Date: 2026-10-17

## Context

ADR-0053 requires source production, join/ON filtering, and WHERE filtering to
remain distinct semantic phases. ADR-0081 added parameterized joined rowid
lookups, and ADR-0096 admitted a bounded arithmetic key domain only when every
ON expression scheduled at the probed source belonged to a no-callback
expression subset. Those decisions preserved correctness while the exact
callback/error schedule remained unmodeled.

The frozen pinned SQLite matrix now contains 640 joined preservation and
error-phase cases: five join spellings, populated/empty left and right inputs,
16 ON predicates, and count/row consumers. Current Modern matches 600 and
differs on 40. The differences form three exact groups:

1. **16 INNER/CROSS readiness differences.** A deterministic source-free ON
   conjunct must run before either source, and a left-owned conjunct must run
   after the left row but before entering the right source. Modern retains the
   complete ON expression at the right physical candidate, so an empty input
   can incorrectly suppress an earlier error.
2. **12 preserved candidate-miss differences.** LEFT/RIGHT/FULL use a computed
   rowid equality such as `b.id=a.id+1000`. Pinned SQLite computes the access
   key, performs the missing seek, and skips sibling ON callbacks before
   publishing unmatched rows. Modern rejects the access candidate when a
   sibling conjunct contains an erroring scalar call, scans physical
   candidates, and executes that sibling too early.
3. **12 preserved computed-key differences.** For
   `a.id<0 AND b.id=abs(a.id-a.id-9223372036854775807-1)`, pinned SQLite
   computes the deterministic scalar rowid key before remaining outer-ON
   conjuncts. Modern retains a scan and conditionally short-circuits the false
   left conjunct, suppressing the key error.

This is not a request to reorder arbitrary expressions. Pinned SQLite gives
each WHERE/ON conjunct prerequisite metadata, uses selected equality terms to
initialize a loop, marks those terms coded, and emits remaining ready terms
at the current loop. Outer-ON ownership prevents unsafe hoisting across the
preservation boundary.

The unchanged 128-by-128 fixed-work image
`phase_rows(id INTEGER PRIMARY KEY,payload BLOB)` has SHA-256
`2d251b94632c46f12c2b68342a609a925bf3cd4d480a83ec5d00c104612e064c`.
The missing deterministic-function probe:

```sql
SELECT count(*)
FROM phase_rows a JOIN phase_rows b
ON b.id=abs(a.id)+1000;
```

returns zero on both engines but executes 131,340 Modern VM instructions,
zero B-tree comparisons, and 13 source-page reads. The already admitted
arithmetic control executes 652 instructions, 768 comparisons, and the same
13 pages. Pinned SQLite executes 652 VM steps for the function form and 524
for the arithmetic control. The Modern function form is still a repeated
inner scan.

## Decision

### 1. Publish one canonical term-level ON schedule

Add `PhysicalJoinPredicatePlacement` to the existing physical nested-loop
join node. Each entry owns:

- the original bound conjunct identity; and
- its stable conjunct occurrence position within the owning ON expression;
  and
- an optional evaluation source, where no source means the pre-open phase.

Retain the original complete ON identity only as provenance. Lowering never
executes that complete expression once placements exist.

Build placements through one optimizer helper:

1. flatten only top-level AND through the existing conjunct visitor;
2. reuse `JoinPredicateSources` for dependency validation, RIGHT/FULL floors,
   and earliest ready source;
3. keep every LEFT/RIGHT/FULL own-ON conjunct at that join's right source;
4. when a later RIGHT/FULL null-supplies an earlier INNER/CROSS left prefix,
   keep each source-ready earlier ON term at its own right/candidate source;
5. place a deterministic source-free INNER/CROSS conjunct in the pre-open
   phase only when the canonical floor is the first source; and
6. preserve original conjunct and join order for terms sharing a phase.

The later-RIGHT/FULL fence applies when a term's ordinary ready source is no
later than its owning join's right source. A genuine forward dependency keeps
its later source assignment.

Do not add a second guard, predicate, or callback collection. Pre-open,
source-ready, candidate, and replay behavior are views of this one placement
list.

WHERE guards and predicates retain their existing `PhysicalFilterNode`
ownership and schedule. This ADR does not move WHERE terms into ON metadata.

### 2. Publish and consume selected access-term identity

Extend the existing physical rowid lookup with the selected original
predicate and conjunct position. Existing stored-index and automatic-index
equalities/ranges already publish that identity.

Lowering uses one `AccessConsumesPredicate` query over the selected physical
source access. A selected equality/range term initializes the cursor and is
not emitted again as an ON residual. Every unselected term executes exactly
once at its published phase.

Consumption matches the owning physical join, predicate identity, and
conjunct position. Expression identity alone is insufficient because alias
expansion can publish repeated ON occurrences that share one expression ID.

Joined access analysis currently visits own-ON terms before eligible WHERE
terms. Preserve that source order and publish whether each candidate predicate
came from ON or WHERE. `AccessConsumesPredicate` applies only while emitting
the ON placement list. A selected WHERE constraint retains the existing
`PhysicalFilterNode` residual recheck and phase; this ADR does not consume,
move, or otherwise reschedule WHERE predicates.

Within ON, choose the first eligible conjunct in original occurrence order,
testing that term against value, arithmetic, and deterministic-scalar key
domains before considering the next conjunct. A later value/arithmetic key
must not bypass an earlier scalar key whose evaluation SQLite performs first.
This supersedes ADR-0096's value-domain preference only for ON-origin terms.
WHERE retains its existing value-before-arithmetic selection.

If an earlier ON rowid equality has a source-ready key outside the admitted
domains, it is an access-order barrier: retain the scan and do not select a
later equality. Otherwise a later missing probe could suppress the earlier
conditional, subquery, concatenation, or nondeterministic expression. A key
whose sources are not ready at the current source is not such a barrier.

Validation reruns the one canonical placement helper and exact-matches the
published vector, including term order, pre-open versus source assignment,
RIGHT/FULL floors, and outer-own forcing. It also reconstructs selected terms
from the original bound conjuncts and requires:

- valid core/source ownership;
- source-ready key dependencies;
- exact predicate/key orientation;
- one selected identity per access constraint;
- no selected ON term emitted as an ON residual;
- unchanged selected-WHERE residual ownership; and
- all original ON conjuncts represented exactly once in the placement list.

No synthetic predicate, copied expression tree, key cache, or runtime plan
interpreter is added.

### 3. Admit a bounded deterministic scalar rowid-key domain

Extend only the joined rowid computed-key classifier to deterministic scalar
calls whose arguments recursively belong to the existing value/arithmetic
domain. Reuse bound function determinism and existing source-readiness
checks. The ordinary expression emitter evaluates the key; the existing
typed rowid seek performs conversion.

This new scalar admission is ON-origin only. Add origin metadata to the
canonical joined access-predicate view and reject a deterministic scalar
candidate discovered through WHERE. Existing value/arithmetic WHERE keys,
their outer barriers, and their complete filter recheck remain unchanged.

Keep these forms ineligible:

- nondeterministic scalar calls;
- aggregate or window calls;
- subqueries;
- conditional expressions;
- concatenation and logical expressions used as key values; and
- any current/later-source dependency.

For ON-origin arithmetic or deterministic-scalar keys, replace ADR-0096's
whole-ON no-callback gate with the canonical term schedule. Keep the existing
gate and behavior for WHERE-origin arithmetic keys. Sibling ON conjunct
safety is represented by placement rather than by rejecting an otherwise
valid ON access key.

This admission applies only to joined INTEGER PRIMARY KEY/hidden-rowid
lookups. Joined stored-index and automatic-index keys remain value-only.
Nonjoined and mutation access contracts remain unchanged.

### 4. Lower the published phases through the existing join producer

The nested-loop producer executes placements as follows:

1. after existing query guards but before opening joined cursors, emit
   pre-open INNER/CROSS terms;
2. after a source row is positioned, emit non-preserving ON terms whose
   evaluation source is that source, before entering later sources;
3. for an outer join's own right source, evaluate the selected access key and
   position first, then emit unconsumed own-ON terms;
4. record LEFT/FULL success and RIGHT/FULL matched identity only after all
   own-ON residuals pass;
5. bind the existing publication label after match recording, so virtual
   LEFT rows and unmatched RIGHT/FULL replay continue to skip own-ON
   evaluation; and
6. emit other ready INNER/CROSS terms after that publication boundary, as the
   existing producer does.

An access miss reaches existing source exhaustion without evaluating
candidate-only residuals. A false/null ready term advances or exhausts through
the current typed labels. Errors remain explicit.

All cursor opening, point/index positioning, outer match flags, RIGHT/FULL
matched relations, null-row publication, noncovering table cursors,
automatic relations, cleanup, and consumer callbacks remain owned by the
current producer.

### 5. Preserve join order and excluded surfaces

Keep declared source order, current join barriers, current access costs, and
all existing table/index/automatic candidates. This slice does not add:

- joined rowid ranges;
- new stored-index or automatic-index function keys;
- join reordering;
- hash/merge joins or Bloom filters;
- a new guard list;
- mutation access; or
- IN, FROM-subquery, CTE, view, trigger, or later feature work.

## Verification and delivery

Before production changes, preserve:

- the exact 600/640 current joined phase result;
- all 600 matching transcripts;
- the 40 differences grouped as 16 readiness, 12 candidate-miss, and
  12 computed-key cases; and
- the 128-row fixed-work image and counters above.

Add focused reds for:

- source-free INNER/CROSS errors with an empty left or right input;
- left-owned INNER/CROSS errors before an empty right source;
- LEFT/RIGHT/FULL missing computed probes that skip sibling errors;
- LEFT/RIGHT/FULL deterministic scalar keys that execute before a false
  sibling;
- access-term non-reexecution;
- exact virtual-row and RIGHT/FULL replay behavior; and
- deterministic versus nondeterministic scalar key admission.

After implementation require:

- 640/640 exact matches in the frozen joined matrix;
- no change to any previously matching transcript;
- function-miss fixed work at no more than 1,024 Modern VM instructions,
  1,024 B-tree comparisons, and 13 source-page reads;
- exact result parity for both fixed-work cases;
- physical-plan tests for pre-open, source-ready, outer candidate, and
  access-consumed metadata;
- exact validation failures for reordered terms, wrong pre-open/source
  assignments, ignored RIGHT/FULL floors, wrong outer-own forcing, and
  ON/WHERE origin confusion;
- INNER, CROSS, LEFT, RIGHT, and FULL count/row consumers;
- populated and empty source combinations;
- table, rowid, covering, noncovering, and automatic-source fallbacks;
- grouped/global/DISTINCT/ordered/set/private scalar/EXISTS consumers;
- LIMIT-zero/early-stop, reset/rebind, OOM, I/O cleanup, and verifier states;
- unchanged nondeterministic callback counts;
- unchanged selected-WHERE residual evaluation and callback/error counts; and
- a pinned native compatibility shard that preserves all inherited records.

Run the complete fast tier and applicable commit-level compiler, sanitizer,
native, fixed-work, formatting, tidy, engineering, and graph gates. Obtain
GPT-5.6 Sol design and implementation review. Commit this as prerequisite
batch ordinal 2, append exact commit/tree/evidence to the ADR-0106 ledger, and
do not open an intermediate pull request.

### Implementation evidence

The canonical term schedule closes the frozen matrix at 640/640 exact
matches. All 600 prior matches remain unchanged. The original 40 differences
now execute at their pinned phases across INNER, CROSS, LEFT, RIGHT, and FULL
count/row consumers.

The committed conditional-predicate shard preserves all 51 inherited
records and adds the 40 former differences, one competing-key-order case,
two excluded-key barrier cases, and four later-RIGHT/FULL fence cases. It now
contains 97 native comparisons and one unchanged unsupported CASE boundary.

On the unchanged 128-row image, the deterministic function miss executes:

- 780 Modern VM instructions instead of 131,340;
- 768 B-tree comparisons instead of zero from the repeated scan; and
- the unchanged 13 source-page reads.

The arithmetic control remains 652 instructions, 768 comparisons, and
13 pages. Both results remain zero and match pinned SQLite. The function form
reduces VM work by 99.4% and passes the accepted 1,024-instruction,
1,024-comparison, and 13-page limits.

The final commit-level fast tier passes 1,374 batched and 89 isolated tests.
Optimizer/lowering OOM and allocation executables pass. Focused ASan/UBSan
tests cover exact placement, single key evaluation, readiness errors, and
outer candidate misses. Formatting, changed-translation-unit clang-tidy,
engineering discipline, graph validation, JSON validation, and whitespace
checks pass.

## Consequences

Positive:

- ON errors and callbacks follow explicit SQLite-aligned readiness and
  preservation phases.
- Selected access predicates are evaluated once.
- Deterministic scalar rowid probes eliminate the remaining repeated-scan
  gap in this matrix.
- One placement representation serves every joined consumer.

Negative:

- Physical join metadata and validation become term-based.
- The shared join producer gains phase iteration around its existing
  preservation labels.
- Deterministic scalar rowid keys enlarge the reviewed access domain.

## Rejected alternatives

### Keep complete ON expressions at the candidate

Rejected because empty inputs suppress source-free/left-ready errors and
selected access terms are reevaluated.

### Hoist all deterministic ON expressions

Rejected because outer preservation requires own-ON terms to remain behind
the candidate access boundary.

### Keep ADR-0096's whole-ON gate

Rejected because one sibling callback can force an otherwise valid computed
probe back to a scan, producing the candidate-miss differences.

### Add separate guard and residual lists

Rejected because duplicate collections drift. One placement list expresses
all ON phases.

### Admit arbitrary scalar or subquery keys

Rejected because nondeterministic, nested, aggregate, conditional, and
logical key forms need separate semantic proof.

## Traceability

Node: `implement-advanced-sql`, prerequisite batch ordinal 2.

Contracts: ADR-0053, ADR-0081, ADR-0085, ADR-0086, ADR-0087, ADR-0089,
ADR-0090, ADR-0096, ADR-0104, ADR-0105, and ADR-0106.

Pinned source:

- `src/wherecode.c:1743-1770` — rowid equality-key evaluation and seek;
- `src/wherecode.c:2650-2735` — ready residual-term emission and coded-term
  suppression;
- `src/wherecode.c:1470-1520` — outer-join readiness rules; and
- `src/where.c:6500-6620` — ON ownership and prerequisite handling.
