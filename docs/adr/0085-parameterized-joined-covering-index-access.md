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

Twenty-eight probes against the pinned SQLite 3.54.0 profile establish:

- ordinary equality, multi-column equality, and IS use a covering prefix;
- a DESC key still supports forward physical prefix traversal;
- numeric comparison against a TEXT index cannot constrain that index:
  `text_key=(SELECT numeric_key FROM probe_rows WHERE id=1)` matches `'1'`,
  `'01'`, and `'0001'`, whereas a TEXT key matches only `'1'`;
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
The same rule covers source-independent affine scalar-query keys before
joined admission. CAST syntax remains outside this slice; its native probe
is additional affinity evidence, not a new admitted surface.

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

Initialize retained key blocks to NULL before any preserving path can bypass
normal positioning. A LEFT virtual row or RIGHT replay must still satisfy
definite register initialization at the conservative normal-loop backedge.
Normal seeks overwrite the block; no verifier relaxation is permitted.

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

At unchanged production base `a1ad89a67eabcbd78c50d1b6aa222c8dc1039371`,
that workload executes 20,503 VM instructions and returns one row containing
the count 1. The focused test observes the missing covering access against
a 256-instruction bound while confirming that result. Initial CAST-based
affinity tests were rejected by the existing syntax boundary and do not count
as behavioral reds; use the already admitted affine scalar query instead.

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

## Closure evidence

The unchanged fixed-work query returns count 1 in 35 VM instructions rather
than 20,503, excluding prepare. Its 256-instruction bound remains intact.
This is a deterministic work reduction, not a wall-time speedup claim.

The complete fast tier passes 1,288 unit cases. Complete Debug, Release, and
ASan/UBSan presets pass 1,334 cases each; genuine GCC 16 Debug and Release
configurations pass 1,332 each. All 58 existing performance-tool, archived-baseline,
CLI, diagnostic-work, and fixture contracts pass. Existing allocation budgets,
timeouts, workloads, thresholds, profiles, and timing baselines are unchanged.

Fifty-four new pinned comparisons bring the matrix to 355 comparisons and
six unsupported boundaries. All 307 previous corpus cases and native records
remain identical as JSON data. The independent indexed fixture covers empty
sources, NULL and duplicate prefixes, mixed affinity and collation, DESC
terms, defaults and REAL fields, complete replay, all current consumers,
typed reset/rebind, and deliberately deferred access paths. DISTINCT uses an
explicit BINARY result collation where NOCASE spelling representatives would
otherwise depend on source order.

An independent ASCII case-fold/NULL model checks 72 combinations of join
kind, equality/IS, own-ON filtering, and post-join WHERE filtering. ON matches
remain consumed when WHERE rejects their output. Public tests retain typed
keys through suspended and completed reset, dynamically locate and fault the
selected index root, skip its reads for LIMIT 0, and recover after failure.
A 4,096-key indexed FULL join proves actual matched-ledger migration, write
failure cleanup, and reset recovery. A public 64-source indexed chain executes;
a separate 64-source FULL topology compiles with one shared consumer and
bounded bytecode without executing the preserved source chain.

Optimizer and lowering sweeps fail every measured allocation boundary, and
three named session sweeps cover indexed FULL aggregation, scalar grouping,
and DISTINCT/set consumption. No previous allocation range is reduced.
Read-SQL fuzz inputs with selector modulo eight equal to five use the indexed
fixture for both raw and generated SQL; the comment-prefixed seed selects
that family. Smoke coverage executes its generated join/key/consumer matrix.
The grammar-aware ASan/UBSan campaign completes 20,000 inputs with seed 85.

Clang-format 23.1.2, production clang-tidy for ordinary and instrumented
libraries, engineering discipline, and dependency-graph validation pass.
Compiler resource identities are captured before emitter calls; already
validated optional resources use the lowerer's canonical `AssumeValue`
invariant helper, without new warning suppressions or weaker guards.
The GPT-5.6 Sol design and implementation reviews report no unresolved
in-scope issue.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 join access/scaling slices.

Contracts: ADR-0033, ADR-0035, ADR-0052, ADR-0053, ADR-0056, ADR-0058,
ADR-0067, ADR-0072, ADR-0073, ADR-0075, ADR-0076, ADR-0077, ADR-0078,
ADR-0079, ADR-0080, ADR-0081, and ADR-0084.
