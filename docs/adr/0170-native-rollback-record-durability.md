# ADR-0170: Native Rollback Record Durability

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Prerequisite: ADR-0169, commit `fc4d27c`, ledger 83
- Audit entry: `storage-rollback-journal`

## Context and completed source map

ADR-0169 repairs cache-only full rollback. It deliberately retains journal
publication after any database-write authorization because the existing
Pager recovery target clears its cache and restores every decoded original
to the database. Removing publication without changing that target would
permit an unsynchronized newer original to reach the database during
rollback, leaving no durable recovery image after a second crash.

The pinned native mechanism is narrower:

| State or operation | SQLite 3.54.0 source | Required translation |
|---|---|---|
| Successful journal sync | `pager.c:4340-4444` | Advance the synchronized boundary only after successful publication and all required sync calls. |
| Later journal epoch | `pager.c:1480-1605,4425-4434` | A later current header does not make its records part of the preceding durable prefix. |
| Main-journal playback eligibility | `pager.c:2459-2475` | A main record may reach the database only when its end is within the synchronized boundary. |
| Cache-only versus database-modified target | `pager.c:2396-2475` | Cache-only rollback does not write database pages. |
| Statement-journal eligibility | `pager.c:2461-2465` | Statement images use the existing cached page's `NEED_SYNC` state rather than a main-record offset. |
| Hot preparation | `pager.c:4088-4112,5295-5421` | Synchronize the opened hot journal before playback; successful preparation makes its readable main records eligible. |
| Live rollback | `pager.c:6830-6863` | Open live playback without synchronizing an unsynchronized tail. |
| Restore and terminate | `pager.c:2872-3050` | Restore only eligible database images, synchronize the database, then finalize the journal. |

SQLite's `journalHdr` combines an active-header boundary with the last
synchronized end. Modern already separates cohort header offsets and
publication flags. One explicit byte offset in the existing journal owner
translates the synchronized-prefix fact without adopting that overloaded
field or introducing a page-indexed durability map.

The same file may contain a durable old cohort and an unsynchronized new
cohort. Safe append can instead append both ranges to one header. A cohort's
publication boolean alone is therefore insufficient to classify records.

## Decision

### 1. Extend the canonical borrowed image, not its byte ownership

Move the existing `RollbackImageDurability` declaration before
`JournalPageImage` and add:

```cpp
std::optional<RollbackImageDurability> main_journal_durability;
```

The existing page number and `ByteView` remain the canonical capture and
playback representation. No second image container, per-page allocation,
owned-byte copy or playback executor is added.

The new field is absent on capture input and statement-journal playback.
Capture must never accept it as authorization; the backend derives main
playback provenance from its own completed synchronization state. A main
transaction or hot-recovery image must carry an explicit valid value.
Missing or invalid main provenance is a typed internal backend-contract
error, not permission to write or successful exhaustion.

This keeps the existing playback and recovery-target method signatures.
The image and its provenance describe the same borrowed record and remain
valid until the next cursor step or cursor destruction.

### 2. Retain one synchronized end in the existing backend owner

The journal implementation owns `synchronized_end_offset`, initially zero.
After `Sync()` fully succeeds, publish the current append end to that
field. Reset it on transaction begin/finalization, and leave the previous
value intact after either sync phase or header-publication failure.

Transaction playback snapshots this boundary. A record ending at or before
it is `Durable`; a later record is `Unsynchronized`. Header geometry,
record-size checks, original-size filtering and checksums remain unchanged.

Safe-append and sequential devices use the same boundary. A successful
device-policy sync can advance it even when the device contract omits an
explicit OS flush. Later safe-append records must not inherit durability
merely because their single cohort was previously published.

Hot playback emits `Durable` only through the existing successful hot
preparation path. It still checks every header's magic and never infers
records from a zero-count unpublished live header.

Savepoint playback transports the same main-record provenance and leaves
statement-journal images unmarked. Its existing pre-playback synchronization,
cache ownership and admission algorithm remain unchanged in this slice.
Removing that separate conservative synchronization requires its own
completed native cache-miss/restore map and reviewed follow-up.

### 3. Consume the proof at the existing target

The shared driver validates main-image provenance before dispatching that
image. It preserves current page validity, size, filtering, duplicate,
error-state and cursor-lifetime rules.

For full rollback, Pager clears its cache as before. A database-modified
target writes only images carrying `Durable`; an unsynchronized image
cannot have authorized the corresponding database overwrite. After cache
invalidation, its original database bytes remain the source of truth.
A cache-only target still performs no database I/O.

For savepoint playback, use explicit main-record provenance when present;
statement-journal playback uses the existing cached `needs_sync` fact.
The retained savepoint synchronization clears those flags before the
unchanged restore path, so this is provenance transport rather than a new
savepoint-admission policy.

Remove the remaining dirty-tail `DoSync()` from live full rollback only
after this proof is carried from the backend to Pager. Commit, spill and hot
preparation retain their original publication and synchronization ordering.
Database restoration, final sync, finalization and terminal cleanup retain
their typed failures and ordering. The existing physical resize mechanism
is not redesigned by this record-durability slice.

## Test-first matrix

Begin with a real Pager mixed-cohort regression on the accepted predecessor:
spill an older protected page, append an unsynchronized newer cohort, arm
the next journal-sync failure, and roll back. The old implementation must
consume that failure. The corrected rollback must leave it unconsumed,
restore the exact original image, and write exactly the durable prefix's
originals rather than the unsynchronized tail.

Owning controls additionally cover:

- initial and later unsynchronized main images;
- durable prefixes followed by ordinary and safe-append tails;
- sequential-device publication;
- failed first sync, publication write and second sync retaining the old
  boundary;
- reuse clearing a previous transaction's synchronized end;
- hot images receiving eligibility only after successful preparation;
- missing or invalid provenance as an internal error;
- unchanged statement/savepoint images, membership, retry and cleanup;
- every existing metadata, checksum, truncated-tail, allocation and typed
  failure prefix; and
- a second crash at every rollback database write/sync/finalization boundary
  while the newer cohort remains unpublished.

Keep all original assertions, corpus records, limits, allocation ceilings,
stack sizes and deadlines. Run the focused red before production changes,
focused green and complete fast units afterward, then independent exact-tree
implementation review and append-only slice evidence.

## Delivery and disposition

This is the second related storage commit in draft #270 under ADR-0106,
after its independently reviewed and sealed predecessor. It does not extend
draft #269 or introduce SQL/compiler/Python fixture changes.

The named full-rollback publication discrepancy can close only after the
complete native/fault/interop/model and exact-head durable closing matrix.
The broader storage audit remains open for any separately recorded
savepoint-publication or physical-resize differences; do not erase those
facts by calling the whole module equivalent.
