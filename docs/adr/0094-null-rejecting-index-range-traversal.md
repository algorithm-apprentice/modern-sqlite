# ADR-0094: NULL-Rejecting Index Range Traversal

- Status: Accepted
- Date: 2026-10-16

## Context

The implemented-engine reference audit found a silent wrong-result defect in
the admitted index range surface. At main
`ebeb974bdc0c935ef627f90b9da076814eee8091`, using the unchanged joined-index
fixture:

```sql
SELECT count(*) FROM key_rows WHERE numeric_key<3;
```

Pinned SQLite returns `4`; Modern returns `7`. The three extra entries have
NULL numeric keys. `<=3` returns `5` versus `8`; ordered scalar demand returns
`1` versus NULL. HAVING-to-WHERE transfer exposes the same defect, not a
separate aggregate bug.

The canonical index analyzer removes selected constraints from residual
filtering. Existing physical start/end comparison correctly implements
record ordering, in which NULL sorts before numeric values, but record
ordering alone does not implement SQL's NULL-rejecting `<`/`<=` predicates.

Pinned `src/wherecode.c:1943-2022,2074-2078,2180-2186` supplies the missing
mechanism: for a nullable upper-only logical range, seek strictly past the
implicit NULL lower boundary, or stop strictly before it when physical
traversal reverses that boundary. Ordinary equality-prefix boundaries and
explicit runtime-NULL rejection remain distinct.

Current reusable mechanisms are:

- `IndexTraversal` and its shared direction/order resolver;
- one explicit-destination index key-block emitter and canonical affinity;
- existing typed index seek and range-end instructions;
- ordinary pooled and joined source-owned key blocks, already sized to
  equality count plus one when a range exists;
- catalog effective NOT NULL and rowid-alias metadata;
- the existing interned NULL constant service; and
- shared ordinary, grouped/global aggregate, joined, and private-query
  producers, including indexed MIN/MAX's separate NULL-pass policy.

No new comparator, executor, predicate collection, register container,
physical node, opcode, persistent format, or cache is needed.

### Resuming the preserved design

The proposed design and its GPT-5.6 Sol review were preserved without
production changes in stash `e1146e97ffd7021a27f0246ff82fc6e6d7c7dd28`.
Resume only this document, not the independent page-view optimization.

The original model deadline correction and durability/lifecycle prerequisites
have now merged as PRs #205, #204, #206, and #207. The current baseline is
main commit `27e8a29a9e26ebde29f962d8745ee6069690ef87`, whose tree is
`24c7054492585d8a3c00c148197bd4b7e34154c3`. Its complete compiler,
sanitizer, native, performance, and nine exact-head hosted gates are green.
The original model passes in 6.33 seconds under its unchanged ten-second
deadline. This range correction is independent of that already closed issue.

Reinventory the unchanged range producers and review the resumed design
against this current baseline before production work.

## Decision

### 1. Represent the implicit boundary in canonical traversal metadata

Extend the internal borrowed traversal metadata to distinguish an implicit
exclusive NULL boundary from an absent expression bound. For an upper-only
logical range on a nullable index term, the missing logical lower bound is
the exclusive NULL boundary.

Use one borrowed boundary view containing the existing expression-bound
pointer or the implicit-NULL fact, never both. The same view is passed to
the existing key-block service. It owns no expression, key, or register and
does not introduce a physical-plan type or a new lowering service.

Use the same resolver for ASC/DESC index terms and forward/reverse scans.
Swapping physical start/end roles also swaps the implicit boundary role.
That boundary counts as a start or end key even when no equality prefix is
present. Its comparison is exclusive.

Make the shared resolver catalog-aware. Derive nullability from the selected
table's existing effective NOT NULL and rowid-alias metadata, rather than
adding independently maintained nullable state to physical descriptors.
Validate catalog table/range-column identities through canonical typed
lowering errors; do not substitute a nullable default for invalid metadata.
Preserve ordinary non-range full scans, NULL-permitting `IS` equalities,
explicit two-sided ranges, and ranges on non-nullable columns.

### 2. Emit the boundary through the existing key block

Extend the canonical explicit-destination key-block emitter to place the
interned NULL constant in the existing range slot when traversal requests
the implicit boundary. Copy existing equality terms normally. Do not create
or evaluate a synthetic bound expression, allocate a second key block, or
copy a private comparison algorithm.

Reuse the current typed seek/end-check modes with exclusive boundary flags.
Do not send this synthetic NULL through runtime-key NULL rejection or range
affinity application: an explicit NULL range parameter still means no match,
whereas the synthetic NULL is a positioning sentinel.

When a NULL end boundary is retained by a joined source, publish it in the
source-owned range slot before the backedge, exactly as for an explicit
range end. An equality-only end check still reuses its retained prefix.
Existing expression homes remain temporary. Preserve the pooled ordinary
block, source acquisition phases, typed cursor-state proofs, and
noncovering first-candidate table-open order.

### 3. Wire every existing range consumer

Ordinary covering and noncovering scans, streaming/grouped producers, global
aggregates, indexed MIN/MAX, and joined ranges must use the same resolution
and key emission. Private scalar/EXISTS consumers inherit the canonical
producer rather than acquiring a special filter.

Keep indexed MIN/MAX's separate all-NULL fallback and explicit equality-NULL
behavior. A NULL fallback may not emit a row excluded by a range. Preserve
representative-row selection, filters, LIMIT/OFFSET, empty input, reset,
rebind, failures, and cleanup.

RIGHT/FULL unmatched replay remains a complete scan of the existing replay
driver and ignores every normal range boundary, including the synthetic
one. Preserve original ON predicates and matched-key ownership.

## Verification

Begin from the green exact-main Debug/integrated and native contract
baseline. Obtain current GPT-5.6 Sol design review before production changes.

Observe one focused wrong-result test on the unchanged joined-index fixture,
including count `4` rather than `7`, before implementation. Then cover:

- `<`/`<=`, reversed comparison orientation, bound absence, and two-sided
  controls;
- ASC/DESC terms and forward/reverse traversal;
- equality prefixes, including `IS NULL`, and composite range slots;
- covering/noncovering rows, COUNT, other aggregates, grouping and pushed
  HAVING, compounds, ordering, LIMIT, scalar/EXISTS, and joined replay;
- explicit NULL parameters, missing ranges, empty/all-NULL inputs, and
  reset/rebind;
- catalog nullability classification: ordinary nullable columns receive the
  sentinel, effective NOT NULL and rowid aliases suppress it, and a nullable
  non-rowid PRIMARY KEY (including `INTEGER PRIMARY KEY DESC`) still receives
  it. Suppression requires traversal/emitted-shape and key-count assertions,
  not only results from data that happens to be non-NULL;
- existing OOM and source-I/O failure cleanup with no relaxed budget;
- independent native vectors without changing old oracle records or fixture
  bytes; and
- the complete fast tier, compiler/sanitizer/static gates, unchanged
  performance/work controls, and exact-tree review.

The main read corpus has reached its existing 512-case cap. Use the same
profile-validated native harness for a bounded separately named index-range
corpus/oracle shard; do not increase the cap, change existing records or
fixtures, add another compatibility runner, or weaken any existing timeout.
Retain that runner's required native-valid unsupported prepare control and
ordinary prepare-error control in this shard as well.

This correction does not close other audit findings or complete node 43.
Broader computed-key admission and predicate evaluation phases remain
separate work.

### Development evidence

Before production changes, the focused public test on the unchanged fixture
observed native count `4` and Modern count `7`; the new native shard also
rejected old production at its first mismatch. The canonical correction
now passes all 35 shard cases: 34 comparisons and one native-valid
unsupported control, including the required ordinary prepare-error control.
The main and subquery corpora/oracles, profiles, limits, and fixture bytes
remain unchanged.

All four catalog classifications pass across both index orders, both scan
directions, and prefix/no-prefix key counts. Effective NOT NULL has no
redundant NULL seek. Retained joined ends are proven to load an actual SQL
NULL constant with no range affinity between seek and end check, remain in
disjoint source blocks, and survive the exact loop backedge. The existing
15-row nested model also runs with lower predicates kept as unary-plus
residuals, selecting upper-only access without changing its logical result.

The 18-case extended focused set passes source-read failure/reset recovery,
malformed noncovering suffixes, complete replay, shared scratch, allocation
checks and eight added exhaustive OOM plans under the existing 256-cut caps.
The fast tier passes 1,424 tests. The current strengthened Debug tree passes
all 1,480 tests, including the unchanged corpora, new verify/regenerate
registration and grammar-aware indexed seed.

An exact replay of the 2,511 frozen cross-feature audit instances resolves
31 previously differing instances, with no new differences and no changed
unresolved outcome. Every replayed native contract remains identical.
These are overlapping test instances, not 31 independent bugs.

The remaining full compiler/sanitizer, fixed-work, native performance,
20,000-input grammar campaign, static/project, exact-tree review and nine
new-head hosted gates remain mandatory before normal merge. No local
focused result substitutes for those delivery conditions.

## Traceability

Node: `implement-advanced-sql`, prerequisite correctness repair.

Contracts: ADR-0035, ADR-0036, ADR-0045, ADR-0052, ADR-0053, ADR-0063,
ADR-0067, ADR-0068, ADR-0085, ADR-0086, and ADR-0087.

Audit: [Implemented-Engine SQLite Reference Audit](../sqlite-reference-audit.md).
