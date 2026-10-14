# ADR-0081: Parameterized Joined Rowid Access

- Status: Accepted
- Date: 2026-10-14

## Context

PR #191 completes executable FULL preservation. Joined producers still scan
every inner table for each accepted prefix, even when its INTEGER PRIMARY
KEY or hidden rowid is equated with an earlier source value.

The existing single-source optimizer already recognizes canonical rowid
identities, and the existing typed seek performs exact SQLite lookup
conversion. Source-owned cursor mappings, LEFT match flags, and RIGHT/FULL
matched-key replay are already shared by every joined consumer.

Pinned SQLite EXPLAIN QUERY PLAN probes show an outer scan and a parameterized
INTEGER PRIMARY KEY search for INNER, LEFT, RIGHT, and FULL equality joins.
RIGHT/FULL additionally perform a complete unmatched-right scan. The
32-by-32 `model_rows` equality workload has 32 matches on both engines but
Modern currently visits the full inner product.

Stored-index join access additionally requires prefix/range positioning,
covering field mappings, comparison-affinity admission, and auxiliary cursor
NULL publication. Automatic covering indexes also require a positioned
prefix-seek contract in the shared ephemeral relation. Neither is bypassed
with a private executor, table-shaped synthetic descriptor, or identity store.

## Decision

### 1. Integrate one coherent positioned-source prerequisite

Allow a joined source to retain its existing physical table scan or rowid
lookup leaf. Keep declared source order and reuse the canonical bound source
identity, physical access descriptors, cursor acquisition, and field mapping.
No syntax, VM instruction, or runtime plan interpreter is added.

Reuse rowid predicate normalization and cost functions. A parameterized key
must be deterministic and depend only on already positioned earlier sources
of its core. A reference to the current or a later source cannot become an
access key. Nonjoined key evaluation remains unchanged.

Prefer an eligible own-ON equality/IS conjunct. WHERE constraints are admitted
only for non-preserved sources in cores with no RIGHT/FULL boundary; retain
the existing conservative outer barriers. In particular, a LEFT `WHERE
right.rowid IS NULL` cannot restrict candidate matching. Retain original ON
and WHERE predicates after the seek.

### 2. Position once per logical prefix

Open each cursor once. At a normal prefix entry, evaluate the key and use
the existing exact typed rowid seek. Missing, NULL, fractional, and out-of-range
keys follow the ordinary source-exhausted edge. A successful point source
produces at most one physical candidate; normal advance clears its logical
position through the canonical typed invalidation and goes to exhaustion
instead of walking subsequent rows. Exhaustion therefore retains the same
definite cursor-state proof as a scan EOF.

LEFT/FULL record ON success before consumers and retain their one unmatched
left extension. RIGHT/FULL keep the existing complete replay scan, ignoring
the parameterized normal-access key. Their mode-dispatched advance continues
the replay cursor normally. Both modes enter the same accepted suffix body
and retain exact matched-ledger and cursor cleanup.

Use one canonical key-evaluation/seek emitter wherever the lowering module
already performs the same rowid positioning operation. Do not duplicate a
conversion algorithm or add per-source key caches.

### 3. Prove bounded work without a timing detour

Record the current deterministic VM-instruction baseline before production
changes. Add a module-owned instrumented equality-join work test that excludes
prepare and keeps the same fixture and SQL before and after the change.
This is fixed-work evidence, not a wall-time speed claim or a change to any
archived timing workload, threshold, or baseline.

At main `bd1925569958caafceb47e6fcf331cf324e8c4ac`, the unchanged read
compatibility fixture and
`SELECT count() FROM model_rows a JOIN model_rows b ON b.id=a.id` execute
5,260 VM instructions after prepare. The focused test observes that red
against a 1,024-instruction point-access bound while confirming the result
count of 32. The independent physical-plan test also observes the missing
rowid lookup before production changes.

Start with a focused physical-plan rejection and the fixed-work red. Cover
source-owned aliases, schema rowids, constant and earlier-source keys, exact
numeric/text conversion, NULL virtual prefixes, forward ON fallbacks,
WHERE/outer barriers, RIGHT/FULL full replay, all existing consumers,
reset/rebind, zero/early LIMIT, failure cleanup, and allocation boundaries.
Retain independent native vectors and the complete fast tier.

Stored-index and automatic-index integration follow this positioned-source
prerequisite in separately reviewed slices. The advanced-SQL node remains
active.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 join access/scaling slices.

Contracts: ADR-0033, ADR-0035, ADR-0052, ADR-0053, ADR-0072, ADR-0073,
ADR-0075, ADR-0076, ADR-0077, ADR-0078, ADR-0079, and ADR-0080.
