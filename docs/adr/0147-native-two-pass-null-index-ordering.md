# ADR-0147: Native Two-Pass NULL Index Ordering

- Status: Accepted
- Date: 2026-10-24
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Confirmed boundary and existing owners

PR #250 delivers native compound scheduling, not every ordered access path.
The unchanged joined-index fixture reproduces another admitted-surface
evaluation difference on merged `6960e93`:

```sql
SELECT id FROM key_rows
WHERE iif(numeric_key IS NULL,abs(-9223372036854775808),1)
ORDER BY numeric_key NULLS LAST LIMIT 1
```

Pinned SQLite returns row `3` and completes through `keys_num_real`.
Modern constructs a sorter and reports integer overflow before any row.
ADR-0145 explicitly left this traversal unimplemented. The reproduced
observable difference now blocks final VM classification; it is not merely
an optional access optimization or permission to close the audit.

| Responsibility | Pinned source / existing Modern owner |
|---|---|
| Match direction, first unconstrained key and nondefault NULL placement | `where.c:5310-5455`; canonical `PathSatisfiesOrder` |
| Admit two passes only without range constraints | `wherecode.c:1993-2025`; `PhysicalIndexScanNode`, `ResolveIndexTraversal` |
| Retain equality keys, NULL sentinel and first-pass counter | `wherecode.c:2032-2150`; existing access-key blocks and lexical register layout |
| Select first/second pass bounds before row evaluation | `wherecode.c:2173-2230`; existing typed index seek/range instructions |
| Switch after first exhaustion, not after LIMIT or top-N rejection | `where.c:99-113,7610-7632`; ordinary, streaming and joined continuations |
| LEFT initialization and RIGHT unmatched replay | `wherecode.c:2008-2011`; existing matched/replay state and cursor capabilities |
| Reset, cancellation, errors and release | Existing VM registers, coroutine ownership and execution cleanup |

The existing `IndexTraversalBound` already models implicit NULL sentinels.
`EmitIndexKeyBlock` owns affinity/copy construction; the typed VM and B-tree
already implement inclusive/exclusive forward/reverse prefix seeks and ends.
The indexed MIN/MAX consumer has a distinct early-stop/null-fallback protocol,
not a reusable full ordered scan. Keep that protocol unchanged and share the
underlying key/seek/end primitives, rather than copying its executor.

## Decision

### Publish the native access property through the current solver

Extend the existing path-order state with a loop-depth mask for nondefault
NULL ordering. Match SQLite's first-unconstrained-column restriction: an
explicit nondefault NULL placement on a later unconstrained index column
does not prove that ordering suffix, even for a NOT NULL column.

The selected stored-index leaf receives one `two_pass_null_order` property.
An actual range constraint excludes NULLs and retains the existing range
traversal without two-pass control, as native code does. Rowid access needs
no NULL partition. Automatic-index ordering remains a separate, undelivered
access boundary; this repair does not silently admit it.

Carry the property through the existing selected loop, physical construction,
validation and explanation. Do not add another order evaluator, candidate
list, cost policy, query owner or source sequence. Preserve bounded path
search, partial ordering, ordered-inner rejection and RIGHT/FULL publication
rules.

### Extend one canonical index traversal

Resolve the partition order from the selected direction and the first
unconstrained catalog index term:

- Logical ASC with NULLS LAST scans non-NULL values, then NULLs.
- Logical DESC with NULLS FIRST scans NULLs, then non-NULL values.

Both physical index directions use the same existing seek/end modes.
Retain the evaluated equality prefix and one trailing NULL in the existing
access-key block. One ordinary register records the pass; joined sources
each own theirs, while sequential sources in a lexical producer reuse the
canonical pooled layout. Coroutine producers retain disjoint layouts.
No expression, callback or equality key is reevaluated between passes.

Use one shared emission control for start, boundary and exhaustion:

1. Start with pass one. A non-NULL-first scan seeks strictly beyond the NULL
   key; a NULL-first scan seeks inclusively to it.
2. On pass-one exhaustion or its partition boundary, select pass two and
   reposition from the ordinary equality-prefix edge (Rewind/Last if empty).
   An empty first partition still reaches the second.
3. Non-NULL-first pass one ends at the equality-prefix boundary; its second
   pass ends inclusively at the NULL key. NULL-first pass one ends
   inclusively at the NULL key; its second ends exclusively there.
4. Only pass-two exhaustion completes the source. LIMIT, scalar demand,
   cancellation and ordered-inner rejection keep their existing final
   continuations and do not restart another partition.

The helper emits through the existing builder and typed instructions.
It owns no cursor, values, runtime frame or secondary interpreter. Centralize
direction/inclusivity conversion with the existing seek/end helpers.
Unflagged accesses acquire no new register, branch or runtime work.

### Preserve cursor and outer-join phases

Keep pass-exhaustion separate from consumer completion and NULL-rejecting
key exits. Normalize cursor capabilities at the shared pass edge before
repositioning; do not weaken verifier joins.

For the newly admitted noncovering two-pass scan, open both cursors before
its first seek and leave the table cursor logically unpositioned before
index advance/bound checks. Every empty, final and error path closes the
same owned cursors. Existing unflagged noncovering emission is unchanged.

Initialize a LEFT source's pass register to zero before equality evaluation
and set pass one only when starting a real scan, matching native code.
Only after both real partitions fail may it publish a virtual row. RIGHT
unmatched replay continues scanning the complete table through its existing
replay branch, without applying normal partition checks. Reentry resets the
pass; Reset, rebind, suspension and cancellation use existing lifetime rules.

## Qualification and delivery

Deliver one coherent optimizer/lowering source after accepted design.
Start with the focused public-session failure above; add owning-module
plan/traversal, lifecycle and failure tests rather than another corpus,
fixture, sorter or benchmark.

Cover both physical directions and partition orders; covering/noncovering
output; empty, all-NULL and no-NULL partitions; retained equality prefixes;
actual range controls; unsupported suffix NULL placement; LIMIT/OFFSET
crossing the boundary; reset/rebind; joined and preserving/replay sources;
scalar/coroutine compound consumers; and existing allocation/OOM paths.
Use native controls from the unchanged fixture to freeze rows and error
phases before implementation.

The green baseline has 1,755 ordinary and 97 standalone fast cases.
Preserve existing oracles, workloads, budgets, caps and deadlines.
Run focused green, complete fast, the actual change-aware closing plan,
native/fixed-work and non-target matched evidence, quality and required
hosted gates. Publish the complete record before exact-tree acceptance and
guarded squash. Remote recovery uses `docs/development-handoff.md` and the
draft PR, not a previous machine's session files.

This repair does not itself close audit entry 12, the later function/parser/
binder entries, node 43 or any later DAG node.
