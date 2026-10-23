# ADR-0144: Native Input-Ordered Row Publication

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Confirmed prerequisite

PR #246 supplies coroutine control, not a native ordered-row producer.
The same blocking-producer mismatch is observable without a compound:

```sql
SELECT id FROM items
WHERE id<=4 AND iif(id=4,abs(-9223372036854775808),1)
ORDER BY id LIMIT 2
```

Pinned SQLite returns `1,2` and completes. Modern reports integer overflow
before returning a row. The reverse-order and genuinely unsorted controls
report overflow in both engines; simply stopping every sorter after two
input rows would be incorrect.

The existing native `items` fixture also confirms joined full-order and
index-prefix block-order mismatches. Those require further producer work;
this bounded first correction does not claim to repair them or the
compound scheduling mismatch.

## Existing and native ownership

| Responsibility | Canonical mechanism |
|---|---|
| Access candidates, selection and direction | `WhereLoopBuilder`, `WhereLoopPlan`, `AnalyzeNonjoinedCoreAccess` |
| Single-source order proof to correct and publish | `WhereLoopBuilder::SourceSatisfiesOrder` |
| Published ordering decision | Existing `PhysicalSortNode::input_order_satisfied` |
| Projection, predicate and cursor traversal | Existing `PlanLowerer` row producer and expression emitter |
| No-sort lifecycle | Existing `HasOrdering`, ordinary row-limit and completion paths |
| Native order proof | `where.c:5150-5495`, shortcut publication at `where.c:6436` |
| Native sorter removal | `select.c:8427-8440` |
| Native row publication | `select.c:1496-1508`, following ordinary LIMIT code |
| Native partial-order buffering | `select.c:750-887`; explicitly outside this first correction |

The current planner chooses ordered single-source candidates and directions,
but discards the result when publishing a normal sort node. Its proof also
incorrectly skips leading source-free deterministic terms; it must be corrected
before becoming a publication contract. Validation insists the marker is false,
and lowering rejects a true marker. The runtime therefore builds a sorter even
when the selected access genuinely supplies the whole order.

## Decision

### 1. Publish the selected access proof, not another ordering algorithm

For an ordinary, nonaggregate, non-DISTINCT SELECT with one table source,
correct and carry the canonical full-order proof from the selected `WhereLoopPlan`
through `NonjoinedCoreAccessAnalysis` to the existing physical sort marker.
Do not infer order again in lowering or introduce a second access search,
ordering representation, comparator, executor or compatibility path.

Preserve native proof boundaries: fixed equality terms retain their existing
affinity/collation checks; index direction and NULL placement must match;
unique rowid or UNIQUE NOT NULL prefixes may satisfy dependent suffix terms.
Before such an order-distinct prefix, only native equality-constrained table
columns are fixed terms. A source-free deterministic expression is not an
independently fixed leading term. For example, `ORDER BY
abs(-9223372036854775808), id` must retain its sorter and overflow, whereas
`ORDER BY id, abs(-9223372036854775808)` may omit the unused suffix after a
unique rowid prefix. The native one-row shortcut is a separate positive case.
Retain the native order-mask bound, with the separate one-row shortcut
handled according to its native contract. A prefix-only or otherwise
unproven match does not become a full-order claim.

Both physical-plan construction and validation consume the same canonical
analysis. A forged true marker on an unordered access remains an explicit
typed invariant error. ORDER BY terms and client output metadata remain
owned by their existing query and plan records.

### 2. Reuse the ordinary row producer when no sorter is required

Treat syntax ordering metadata and an active sorting operation as distinct.
Use the existing `HasOrdering` decision consistently for sorter/top-N
registers, descriptors, insertion, draining and source-row LIMIT handling.
An input-ordered plan allocates no unused sorting capability.

At the existing ordinary row-publication point, evaluate the bound result
expressions through the canonical projection helper and publish the normal
result block. Do not evaluate discarded hidden ordering expressions merely
to simulate a sorter that native removed.

OFFSET, LIMIT initialization, zero/negative limits, parameter rebinding,
schema validation, cursor positioning and cleanup retain their canonical
paths. A full-order scan stops at the same LIMIT boundary without visiting
later predicates or projections. A required sorter keeps its existing
admission, full-input evaluation and error behavior.

Scalar and EXISTS destinations use the same producer and their accepted
projection/single-row-demand rules. No nested session, copied query owner,
runtime compiler or coroutine-only expression implementation is added.

### 3. Keep remaining producer and compound corrections explicit

This first correction does not admit FROM, IN, CTEs, views or other new SQL.
Joined ordering proofs, partial-order block flushing, non-natural NULL
traversal and native compound-stream composition remain separately ordered
work. Do not describe a conservative false negative in those paths as
native execution parity or use it as a blocking fallback in the completed
compound correction required by ADR-0143.

Subsequent design must preserve native balanced UNION/UNION ALL splitting,
mixed-operator association, alternate-collation outer sorting, duplicate
representatives, one-row lookahead and LIMIT/OFFSET scheduling. The initial
frozen compound row/error matrix remains authoritative.

## Development and evidence

Start with the public-session row/error regression above and observe its
failure before changing production. Cover forward/reverse rowid and index
access, fixed and unique prefixes, collation/NULL mismatches, order-mask
boundaries, leading hidden errors versus order-distinct suffixes and one-row
shortcuts, LIMIT/OFFSET, scalar demand, reset/rebind,
forged plan metadata and allocation/failure cleanup in the owning modules.
Preserve existing frozen oracles and preparation allocation budgets.

The qualified same-tree PR #246 native capture is the before baseline.
Use unchanged matched workloads and diagnostics; do not claim a speedup
before exact configuration, fixed work and repeatable timings establish it.
Run focused and complete fast units for each accepted source, conservative
change-aware closing, original deadlines, actual GNU/hosted requirements,
and GPT-5.6 Sol review. Node 43 and audit entry 12 remain incomplete.
