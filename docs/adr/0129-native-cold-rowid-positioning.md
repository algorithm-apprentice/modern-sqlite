# ADR-0129: Native Cold Rowid Positioning

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, table-position prerequisite

## Required nearest-position contract

Native saved-cursor restoration requires one traversal to a nearby table
entry, with a signed comparison to the saved rowid. GE followed by Last is
not equivalent: it can repeat the root walk and changes work and failure
ordering.

Pinned `sqlite3BtreeTableMoveto()` defines that contract at
`btree.c:5815-5995`. Its cold per-page search uses inclusive bounds and keeps
the last compared cell. A failed exact search can leave either the
predecessor or successor. Interior equality descends through that separator's
child slot; leaf equality returns immediately.

An independently executed native control on keys `{2,4,6}` records:

| Target | Positioned rowid | Comparison |
|---|---:|---:|
| minimum signed rowid, 1 | 2 | +1 |
| 2 | 2 | 0 |
| 3 | 2 | -1 |
| 4 | 4 | 0 |
| 5 | 6 | +1 |
| 6 | 6 | 0 |
| 7, maximum signed rowid | 6 | -1 |

Modern read and writable cores currently duplicate a lower-bound search.
The writable result always prefers the successor when available; the read
API applies a requested bound immediately and does not retain a nearest
result. Neither is the canonical primitive required by native restore.

## Decision

### 1. One pure per-page table search

Add one `BtreePageView::SearchRowid(std::int64_t)` operation and a small
result containing cell index, insertion/child slot, and signed comparison.
The insertion slot is also the child slot on an interior page.

Translate native's unbiased cold inclusive-bounds search, including early
equality and the last-compared candidate. Use the existing validated
`cell()`/rowid decoder, not another varint reader. Preserve typed errors for
non-table pages and malformed cells. An empty page returns the existing
empty-table comparison (-1) and zero indices, which callers must not
dereference.

Arithmetic is standard-defined: decoded cell counts fit the on-disk
16-bit bound and search indices use an adequate signed type; rowid
comparison does not subtract signed keys. No file I/O, allocation, page
ownership, record movement or catalog mutation belongs in this operation.

This is the one shared search, not an additional optional algorithm.
Remove both existing per-page lower-bound loops.

### 2. Reuse it in both physical table cores

The read core gets one private nearest-position operation: reset to root,
search each page once, descend through the returned child slot, and retain
the leaf candidate/comparison. Preserve existing depth, kind, corruption,
snapshot and pin validation.

Its public bound-seek adapter uses that nearest result and at most one
existing Next/Previous transition for Equal, GE, GT, LE or LT. No fallback
second root walk and no second candidate-search implementation is allowed.
Unsuccessful bounds retain the existing public invalid-position contract.

`WritableCursor::SeekTable()` uses the same page search and retains the same
candidate/comparison, insertion slot and depth. Its physical nearest choice
changes intentionally to native's choice; ordinary insertion order and
public SQL results remain unchanged.

Existing optional instrumentation records actual comparisons in the shared
search for both roles. Remove the old reader-only increments; do not leave
duplicated counts or hide writable comparisons. Instrumentation remains
compiled out of the ordinary library. Report the resulting fixed-work
counter changes rather than adjusting workloads or limits.

## Test-first and delivery

Before implementation, add a focused native-nearest missing-key regression
that demonstrates the current writable successor differs from native's
predecessor for target 3. Add independent page-level vectors for leaf and
interior pages, equality, gaps, empty pages, signed extrema, wrong kinds and
malformed rowid cells.

Verify all five public seek modes in both directions and cross-page
boundaries, including virtual roots and existing depth/error/OOM controls.
Preserve mutation golden images, insertion/deletion continuation, ownership,
allocation ceilings and original model deadlines.

Run focused green, complete freshly linked fast units, affected GNU/
sanitizer and quality checks, native/matched controls and exact-tree review.
Commit and ledger this second related source slice before actual peer
save/reseek begins.

## Explicit boundary

This translates the cold path required by restore. It does not add unused
append hints or independently optimize warm last-key/AtLast/successor
shortcuts; those retain explicit later VM/cursor attribution and matched
profiling requirements.

The following accepted design must reuse this primitive for real table peer
save/release/restore/skip and the dependent VM moved-record, rowid and deleted-
row NULL contract. Index keys, schema destruction, rollback/fault policy,
overflow caching, lazy Column and new SQL remain later work. No position
saving is claimed merely from this search prerequisite.
