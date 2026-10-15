# ADR-0086: Parameterized Joined Noncovering Index Access

- Status: Accepted
- Date: 2026-10-15

## Context

PR #196 completes ADR-0085's covering equality prerequisite. Node
`implement-advanced-sql` remains active. A joined source still scans its table
when an otherwise eligible stored equality index lacks a required field.

The canonical implementation already provides:

- immutable equality candidates, covering discovery, STAT1 costs, and stable
  preference in the physical optimizer;
- an index descriptor whose field zero reads the physical rowid suffix;
- a table descriptor with ordinary fields, defaults, rowid aliases, and REAL
  affinity;
- exact typed index-rowid table positioning and corruption reporting;
- source-owned prefix registers and logical source-to-field mappings;
- typed virtual NULL rows and exact cursor-state verification; and
- one shared joined suffix body and complete RIGHT/FULL replay.

Nineteen pinned SQLite 3.54.0 probes cover noncovering INNER, LEFT, RIGHT,
FULL, IS, two-term keys, residual fields, defaults, REAL values, all existing
destinations, and the unchanged 4,096-row indexed work fixture. Native normal
entry uses an index search and table positioning. Noncovering RIGHT/FULL
replay scans the complete table, unlike covering replay through the index.
Retain that table replay order rather than silently changing first-row or
order-sensitive aggregate behavior.

## Decision

### 1. Extend the existing equality candidate, not its admission language

Admit noncovering candidates with the same ordinary rowid-table shapes,
nonempty forward `=`/`IS` prefix, value-only keys, earlier-source readiness,
comparison affinity, collation, predicate phases, and preservation barriers
as ADR-0085.

Remove only the covering-only admission gate. Reuse the same candidate
analysis, published noncovering lookup costs, stable preference, and physical
validation. Preserve rowid point-lookup priority and unselective STAT1 scan
fallback. Covering access continues to use one cursor.

Parameterized ranges, computed/function keys, automatic transient indexes,
skip-scan, join reordering, and unsupported index shapes remain later slices.
No new syntax, opcode, VM executor, or physical-plan interpretation is added.

### 2. Retain two physical cursors in one source resource

Keep `JoinedSourcePlan::cursor` as the driving access cursor. An admitted
noncovering source adds one optional companion table cursor. Reuse the
existing descriptor builder rather than synthesize another table or column
mapping.

Publish the companion table through the canonical logical source binding.
All declared fields and rowid expressions therefore read the same table
descriptor, including defaults and REAL affinity. Do not introduce a hybrid
field map or a second source-value container.

Open both capabilities once after the existing LIMIT/guard bypass. Root
validation stays in the existing joined source-acquisition phase; do not add
a lazy-root cursor mode or weaken `CursorCore::Open`. Ordinary and aggregate
noncovering access retain their existing first-candidate table-open phase.
Close both handles on ordinary completion and early stop; VM-owned cleanup
retains all error, reset, finalize, and destruction paths.

Retain the source-owned prefix block from ADR-0085. Reuse the existing query
index-rowid scratch register for immediate index-to-table positioning; it is
not retained across a consumer or nested query call. Allocate it once when
any joined or ordinary source in that query requires it.

### 3. Share the canonical index-to-table emission everywhere

Extract the existing rowid-field read, optional first table open, and exact
`SeekTableRowIdInstruction` emission into the existing internal lowering
helper boundary. Ordinary, grouped, global aggregate, indexed MIN/MAX, and
joined callers use the same operation.

Preserve existing instruction order, error mapping, and first/subsequent
candidate phases. The helper owns no runtime cursor or plan and allocates no
container. Do not add another access module or a virtual service merely to
append these existing typed instructions.

On normal joined entry, seek and bound the existing index prefix, then
position its companion before evaluating own ON, later predicates, or any
consumer that can read table fields. A missing or noninteger suffix and a
suffix with no table record remain typed corruption, never a skipped row.

### 4. Preserve exact NULL and exhaustion states

An unmatched LEFT/FULL row publishes virtual NULL on both physical cursors.
The logical source binding reads the companion, so NULL suppresses stored
defaults and REAL values exactly as it does for an ordinary table source.

Invalidate the companion through the existing typed NULL-row disable before
normal index advance. Every normal candidate backedge then carries an
unpositioned companion, while accepted and virtual rows carry a positioned
companion. Key misses, NULL rejection, prefix ends, and EOF retain the
existing exact unpositioned exhaustion proof.

Synthetic prefixes for RIGHT/FULL replay publish and clear both handles.
Do not merge incompatible cursor states, relax verification, or read an
unpositioned companion to recover a source identity.

### 5. Replay the complete noncovering table through the same suffix body

Normal matches retain their existing ON-before-ledger-before-WHERE ordering.
Read matched identities through the canonical logical source rowid emitter,
after exact table positioning has validated the index suffix.

During unmatched replay, rewind and advance the complete companion table.
Read its real rowid, consult the same matched-key relation, and enter the
same accepted suffix body without reevaluating own ON. Replay includes rows
outside every normal index prefix and preserves the native table order.

The unused driving index is parked in the existing typed virtual-NULL
position while the companion table drives replay. This keeps the shared
accepted body definitely positioned for both capabilities without pretending
that an index record supplied replay data. All logical fields and rowids
still read the real table cursor. The replay-mode branch advances the table,
never the parked index.

Normalize the parked index to unpositioned at a dedicated noncovering replay
exhaustion edge before joining ordinary exhaustion. Allocate that extra
continuation only for noncovering replay; keep covering and index-free
allocation behavior unchanged. No suffix is copied, and no new ledger or
runtime execution mode is introduced.

## Verification

The bounded GPT-5.6 Sol design review found no unresolved in-scope issue
before production changes.

Begin from merged prerequisite
`0e2e329c2f0f60789e9ba2fcbfc876c620231ca9`. Observe a missing noncovering
physical leaf and a fixed-work red before production changes.

Use the unchanged indexed fixture and SQL:

```sql
SELECT count(b.payload)
FROM items a JOIN items b ON b.category=a.category
WHERE a.id=1
```

Exclude prepare, retain result count 1, record actual before/after VM work,
and use a new 256-instruction bound without modifying the existing covering
workload or any frozen performance input, budget, threshold, or baseline.
This is fixed-work evidence, not a wall-time speedup claim.

Reuse the independent indexed fixture and append native evidence without
changing any existing corpus case or oracle record. Pin table-backed fields,
defaults and REAL types, nested retained prefixes, actual-core ownership,
unselective costs, both-cursor resource acquisition and failure, malformed
suffixes, complete table replay and its order, matched-but-filtered rows,
NULL virtual prefixes, all consumers, reset/rebind, zero/early LIMIT, and
bounded 64-source compilation/execution.

Extend canonical module OOM sweeps without reducing allocation ranges or
caps. Keep the resource-locked combined session partitions and per-test
timeouts. Add independent models and grammar-aware sanitizer fuzz. Run the
complete fast tier, required compiler/sanitizer/quality gates, and unchanged
archived performance contracts.

Review this design before implementation and design/code closure before
committing production. Merge only the reviewed exact head after all required
local and hosted gates pass, then continue the next ready advanced-SQL slice.
This package does not complete node 43.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 join access/scaling slices.

Contracts: ADR-0033, ADR-0035, ADR-0036, ADR-0052, ADR-0053, ADR-0067,
ADR-0072, ADR-0076, ADR-0077, ADR-0079, ADR-0080, ADR-0081, ADR-0084,
and ADR-0085.
