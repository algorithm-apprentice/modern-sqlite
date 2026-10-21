# ADR-0131: Native Cold Index Positioning

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, index-position prerequisite

## Reproduced nearest-position difference

PR #237 establishes actual table peer positions, not index restoration.
An index also needs one nearest-position traversal with the native signed
comparison before its encoded key can be restored correctly.

Pinned `sqlite3BtreeIndexMoveto()` specifies this at `btree.c:6039-6307`.
Its cold search uses inclusive bounds, early equality and the last compared
candidate. Equality on an index interior page is a real entry; unlike a
table separator, it does not descend.

Independent native and Modern controls on encoded index keys `{2,4,6}` show:

| Search prefix | Native candidate / comparison | Current writable candidate / comparison |
|---|---|---|
| 3 | 2 / -1 | 4 / +1 |
| 5 | 6 / +1 | 6 / +1 |

Modern read and writable cores currently duplicate a lower-bound loop.
The read adapter immediately applies a bound, while the writer always
prefers a successor when present. GE followed by Last is not a native
nearest primitive: it can repeat the root walk and change failure ordering.

The controls also confirm that native index peers save owned raw key bytes,
release every path page and resume at the inserted successor, while the
current Modern reader causes `kBusy`. Those following behaviors are not
claimed by this cold-search prerequisite.

## Decision

### 1. Share one native-shaped index search

Add one private B-tree-layer `SearchIndexCells` helper over a cell count and
the existing typed candidate-comparison callback. Both physical cores use
it; remove both old index lower-bound loops.

Preserve native candidate order, early equality, the final signed comparison
and the independent insertion/child slot. Empty input returns the existing
empty-tree comparison (-1) and zero indices, which callers do not
dereference. Standard-defined index arithmetic must handle zero and the
validated page cell-count bound without signed overflow or unsigned
underflow.

Reuse the existing three-field page-search result, renaming
`BtreeTableSearchResult` to `BtreePageSearchResult` rather than introducing a
second equivalent result type. The table search's algorithm is unchanged.

The index loop belongs in the B-tree layer because its existing comparison
callback may read an overflow key. Do not put Pager/VFS callbacks or I/O
into the persistent-format page decoder. There is no new comparator,
decoder, record representation, cursor container or execution path.

### 2. Position once, then apply the public bound

The read core gets one private cold nearest-index move: reset through its
canonical root loader, search each admitted page once and descend through
the returned child slot. Retain an exact interior entry or the final leaf
candidate/comparison.

The existing public Equal/GE/GT/LE/LT adapter preserves
`EqualPrefixResult`, including the native default-result bias used to find
the beginning or end of an equivalent prefix. Apply at most one existing
Next/Previous step to the nearest result. Remove superseded local candidate
adapters rather than keeping another positioning algorithm.

The writable seek retains the same candidate, signed comparison, insertion
slot, exactness and tree depth. Ordinary insert/delete callers continue to
consume their existing typed result.

Keep root/kind/depth/snapshot checks, malformed-key results, operation-local
scratch ownership and failure cleanup. Reuse the current record comparator
and ADR-0088 bounded-prefix/full-chain error-visibility contract; this slice
does not redesign overflow acquisition or its error precedence.

Record seek-candidate comparison attempts once in the shared search for both
roles. The current reader increment also serves `CompareCurrent()` and
`CompareCurrentRecordPrefix()`: retain one explicit increment at each of
those non-search boundaries before removing the increment from their shared
comparison helper. Seek must not count again inside that helper, and current
comparison APIs must not disappear from the metric. Instrumentation remains
compiled out of the ordinary library. Disclose changed counts, not adjusted
workloads or limits.

### 3. Keep the following mechanisms explicit

This is the cold prerequisite used after a saved position becomes Invalid.
It does not independently add unused warm last-page/last-key shortcuts or
bulk hints. Their remaining attribution, like ADR-0129's table shortcuts,
stays explicit.

Index metadata ownership and encoded-key saving/restoration remain following
accepted slices. The completed mapping must preserve `KeyInfo` ownership,
active comparison width and `nKeyField + 1` unpacking rather than assuming
that every stored covering payload field participates in restoration.
The executed top-N control has one native key field, four stored fields and
two unpacked restore fields; a non-unique persistent index has two key/all
fields including rowid. Do not infer these domains from stale comments.

No index peer fence, schema destruction, global rollback/fault policy,
overflow page list, lazy Column, new SQL or later DAG node is enabled here.
Continue toward real index positions in the same bounded related batch;
this prerequisite alone is not index-position or ledger-12 completion.

## Test-first qualification

Before production, add the real writable gap regression and record the
expected successor-versus-predecessor failure. Test callback visitation,
equality, empty and boundary candidates, insertion slots and first-error
propagation through the shared helper.
Instrument scoped seek, current-value, current-record-prefix and writable
seek controls to prove each comparison path counts once without duplicate
seek increments or omitted non-search work.

Use real encoded records for leaf/interior equality, both gap signs,
prefix duplicates, all five bound modes, forward/reverse traversal,
descending/NULL/collation ordering and cross-page boundaries. Retain current
malformed, depth, snapshot, overflow/I/O/OOM, scratch-lifetime and mutation
golden/interoperability controls.

Run focused green, a complete freshly linked fast tier, affected isolated
GNU/sanitizer/instrumented and changed-production-TU quality checks.
Compare unchanged native/matched workloads against the qualified PR #237
baseline. Preserve all assertions, allocation ceilings and original model
deadlines. Obtain exact-tree GPT-5.6 Sol implementation acceptance and append
the complete slice ledger before the next source slice begins. Final batch
closing and all nine hosted gates remain mandatory under ADR-0106.
