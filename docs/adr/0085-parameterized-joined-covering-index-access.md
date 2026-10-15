# ADR-0085: Parameterized Joined Covering Index Access

- Status: Accepted
- Date: 2026-10-15

## Context

PR #195 completes uncorrelated scalar and EXISTS consumers. Node
`implement-advanced-sql` remains active. ADR-0081's rowid-positioned producer
is merged, but joined sources still ignore ordinary stored indexes.

The canonical implementation already provides physical index candidates,
STAT1 costs, covering-field discovery, typed prefix seeks and end checks,
index-record rowids, NULL-row publication, and complete RIGHT/FULL replay.
Integrate these capabilities rather than introduce another executor or an
index-shaped synthetic table.

Twenty-six probes against the pinned SQLite 3.54.0 profile establish:

- ordinary equality, multi-column equality, and IS use a covering prefix;
- a DESC key still supports forward physical prefix traversal;
- numeric comparison against a TEXT index cannot constrain that index:
  `text_key=CAST(1 AS NUMERIC)` matches `'1'`, `'01'`, and `'0001'`, whereas
  a TEXT key matches only `'1'`;
- source-column affinity, not the runtime value alone, determines that
  admission; comparison orientation does not change it;
- RIGHT/FULL uses the normal equality search plus a complete unmatched-right
  scan, including rows outside every normal prefix;
- real affinity, indexed default values, NULL IS keys, and NULL extension
  retain their ordinary column semantics; and
- later ON clauses can require an otherwise unselected payload column.

The pinned amalgamation's `sqlite3CompareAffinity` and
`sqlite3IndexAffinityOk` at `sqlite3.c:113141-113194` define the common
comparison/index-affinity rule. ADR-0052's shared matcher must enforce it for
ordinary, aggregate, and joined access, not through a join-only heuristic.

## Decision

### 1. Deliver one coherent covering equality prerequisite

Admit nonempty leftmost `=`/`IS` prefixes of existing supported ordinary
rowid-table indexes when the complete joined source is covering. Retain ASC,
DESC, registered collations, UNIQUE/nonunique keys, and the existing
unconstrained table-scan fallback.

Use the existing physical index node, index candidate analysis, cost
publication, and stable candidate preference. Preserve an already eligible
rowid point lookup. Do not reorder sources, add new syntax or bytecode, or
broaden partial/expression/WITHOUT ROWID secondary-index eligibility.

Parameterized noncovering and range access, automatic transient indexes,
skip-scan, and computed-key admission remain separately reviewed later
slices. Their absence is not a new unsupported-SQL error.

### 2. Share constraint admission and preserve predicate phases

Reuse ADR-0081's value-only key classification and earlier-source readiness:
literals, parameters, declared columns, rowids, transparent wrappers, and
coalesces containing only those values. A key cannot require its own source,
a later source, or an outer query. Keep computed/function keys on their
existing scan path.

Prefer eligible own-ON conjuncts. WHERE constraints are eligible only for a
non-left-preserved source in a core without RIGHT/FULL. Keep the complete
original ON and WHERE evaluation and their readiness/preservation barriers;
an access constraint does not become a second guard/predicate schedule.

The canonical matcher also checks effective comparison affinity. No-affinity
comparisons permit any supported index affinity. TEXT comparison requires a
TEXT index term; numeric comparison requires NUMERIC, INTEGER, or REAL.
Keep effective collation matching and indexed-operand transparency unchanged.
The same rule corrects source-independent CAST keys before joined admission.

### 3. Discover covering requirements per actual core

Reuse the existing source-value walker and physical index-field mapping.
For a joined core include all its result columns, ON clauses (including later
sources), WHERE, applicable query ordering, aggregate inputs, and retained
representative leaves. Do not substitute root-client metadata for a later
compound core or a nested query.

The implicit rowid suffix covers hidden rowids and their INTEGER PRIMARY KEY
aliases, including matched-ledger identities. A missing payload field keeps
the existing table scan. Physical validation recomputes the same canonical
candidate and checks the complete selected metadata.

### 4. Retain prefix state in the existing source resource layout

Each selected joined index source receives its own contiguous key-register
block in `JoinedSourcePlan`. It is compile-time resource metadata, not a
runtime key cache or another value container. Nested source loops and child
query regions cannot overwrite a parent's retained end key.

Generalize the existing index-key block emitter to take an explicit
destination block. Ordinary and aggregate callers keep their existing
workspace and evaluation phase. Evaluate each joined key once per logical
prefix, in original conjunct order, and copy/apply affinity in physical key
order through that same emitter.

Open one ordinary index cursor per selected source. Reuse the canonical
covering descriptor and source field/real-affinity mapping. Both ordinary
rowid expressions and preservation-ledger identities use the same
cursor-owned rowid/record-field emitter; do not emit table rowid instructions
against an index cursor.

### 5. Preserve exact exhaustion and complete outer replay

Normal entry uses the existing typed prefix seek. A NULL-rejected or missing
key follows the ordinary unpositioned exhaustion edge. Each candidate uses
the existing prefix end check and source predicate/body. An out-of-prefix
position clears through typed NULL-row disable before exhaustion, keeping
the existing exact verifier proof. EOF and virtual-row advance retain their
ordinary behavior.

LEFT/FULL records ON success before consumers and publishes the existing one
NULL row on an unmatched prefix. RIGHT/FULL replays the complete selected
index with ordinary rewind/next, ignoring normal prefix keys and end checks.
Both modes enter the same accepted suffix body. The canonical matched-key
relation, source order, synthetic-prefix publication, and cleanup are
unchanged.

Every consumer, including ordering, membership/compounds, aggregation, and
scalar/EXISTS private subroutines, uses this producer. Retain zero/early LIMIT,
suspension, reset/rebind, failure, and destruction behavior. There is no new
persistent transition, cursor kind, verifier relaxation, or runtime compiler.

## Verification

The bounded GPT-5.6 Sol design review found no unresolved issue before
production changes. The native audit and canonical-mechanism inventory
precede the test-first implementation.

Begin with a missing physical-plan selection and an instrumented public
session fixed-work red before production changes. Measure the unchanged
4,096-row indexed fixture and
`SELECT count() FROM items a JOIN items b ON b.category=a.category WHERE a.id=1`,
excluding prepare, before and after implementation. Record actual VM work and
the unchanged result count; do not claim timing improvement or alter any
frozen benchmark input, threshold, allocation budget, profile, or baseline.

Add an independently generated small native fixture for mixed affinity,
collation, NULL/duplicate prefixes, indexed defaults, and REAL fields. Pin
covering/missing fields, later ON dependencies, compound-core ownership,
multiple retained prefix blocks, complete outer replay, USING/coalesced keys,
all consumers, NULL virtual prefixes, parameter reset/rebind, and bounded
64-source compilation.

Cover typed source/index and temporary-storage failures, exact cleanup,
exhaustive module-owned OOM boundaries, independent models, native vectors,
and grammar-aware sanitizer fuzz. Preserve all prior native records and run
the complete fast tier, required compiler/sanitizer/quality gates, and
unchanged archived performance contracts.

Review this design before implementation and design/code closure before
committing. Merge only after all local and hosted gates pass; then continue
the next prerequisite-ready slice without completing node 43 prematurely.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 join access/scaling slices.

Contracts: ADR-0033, ADR-0035, ADR-0052, ADR-0053, ADR-0056, ADR-0058,
ADR-0067, ADR-0072, ADR-0073, ADR-0075, ADR-0076, ADR-0077, ADR-0078,
ADR-0079, ADR-0080, ADR-0081, and ADR-0084.
