# ADR-0169: Native Live-Rollback Publication Boundaries

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Audit entry: `storage-rollback-journal`

## Context and corrected design assumption

ADR-0040 explicitly requires synchronizing a dirty main journal before every
live full rollback. ADR-0041 then shares one streaming reader between live
rollback and hot recovery. Although it documents the live zero-record-count
case, the implementation still requires published magic for the first and
every later header. The synchronization makes that stronger precondition
true.

This is a documented conservative mechanism, not an absence of design.
It is nevertheless different from the pinned SQLite algorithm. Existing
recovery/result assertions and the completed statement-journal audit did not
prove the live-rollback I/O budget. Classifying the whole main-journal
mechanism as reference-faithful was too broad.

The local native MSVC Release comparison at `8b714ea` uses the same production
sources as main `d5ef969`: 4096-byte pages, 512 cache pages, memory temporary
storage, DELETE/FULL journaling, normal locking and single-threaded execution.
A transaction inserts 256 rows without spilling and then rolls back.
Both engines issue zero database writes; Modern issues two journal syncs and
SQLite issues none. Three uninstrumented pairs have median complete-work
times of 4.387 ms and 1.666 ms respectively. Separate counter runs and pinned
SQLite reads verify the result; instrumented timings are not the baseline.

Prepare allocation costs and execution-time schema admission are separate
findings. ADR-0151 explicitly accepts immutable compiler boundaries and
verification costs. This storage repair does not remove those contracts,
introduce an allocator, change SQL compilation, or claim to fix the unrelated
Python-only qualification timeout.

## Completed native mechanism map

The oracle is the local pinned SQLite 3.54.0 source from the manifest.

| Native owner or transition | Source | Required invariant |
|---|---|---|
| Zero-magic initial main-journal header | `pager.c:1500-1605` | Before publication, ordinary live header magic and record count are zero. |
| Read a header during live versus hot playback | `pager.c:1627-1705` | Hot playback checks magic. Live playback may bypass magic only at its current unpublished header boundary. |
| Publish and record the synchronized boundary | `pager.c:4340-4444` | Successful sync advances `journalHdr` to the journal end; a later header starts a new unsynchronized cohort. |
| Select cache-only versus database rollback | `pager.c:6830-6863` | Cache-only rollback does not publish the main journal. |
| Infer the current live cohort's zero record count | `pager.c:2872-2970` | Only the current live zero-count cohort derives complete records from the available journal extent. |
| Choose which originals may reach the database | `pager.c:2317-2535` | Main-journal record position and the synchronized boundary determine write eligibility; cache-only state never writes the database. |
| Synchronize restored database bytes | `pager.c:3000-3035` | Only database-modified or hot recovery state needs database synchronization before finalization. |
| Keep finalization after successful playback | `pager.c:2090-2210,3000-3050` | A failed restore or finalization cannot publish successful rollback. |

Modern already has the required no-database-write fact:
`JournalTransaction::database_may_be_modified_` becomes true before an
authorized database write or resize. Pager independently retains actual
write/resize history and makes a cache-only recovery target perform no
database I/O. Both facts are retained on errors. No second page-membership
set, write-history map, rollback executor or journal parser is needed.

The current full rollback target clears the cache and writes all decoded
images when database bytes may have changed. Therefore merely removing the
pre-playback sync for every rollback would be unsafe: a newer unsynchronized
cohort must not acquire database-write eligibility through an earlier spill.
That dependent boundary requires its own reviewed follow-up before the
storage audit can close again.

## Decision

### 1. First slice: remove publication only from cache-only full rollback

When `database_may_be_modified_` is false, `JournalTransaction::Rollback`
opens live playback without `DoSync()`. When it is true, the existing
pre-playback synchronization remains until the dependent record-boundary
repair is qualified.

No page has been authorized to change persistent database bytes in the first
case. The existing Pager recovery target clears its cache and restores
transaction-start metadata without writing, resizing or synchronizing the
database. Its journal can then be deleted after successful playback without
first becoming a crash-recovery artifact.

Success clears dirty-tail bookkeeping because the journal has been finalized;
it must not manufacture `journal_ever_synced`. Cursor-open errors remain
retryable before target preparation. Target, decoding and finalization
failures retain the current persistent-error rules and original errors.

### 2. Carry only the actual unpublished-header boundary into the reader

Use the existing cohort owner and playback kind. A live header exemption is
present only when the final cohort is unpublished. A previously published
cohort is not exempt merely because it is the last cohort.

One shared reader predicate applies that exemption to the first-header,
later-header and zero-record-count paths. Every non-live header and every
hot-recovery header still requires magic. Geometry, size-admitted metadata
reads, checksum validation, original-size filtering and tolerated incomplete
page-record tails retain their current checks.

Safe-append headers already contain magic and use `0xffffffff`; sequential
devices retain their existing publication rules. Failed append or sync
operations do not publish a successful boundary. The reader does not infer a
new boundary from cached page flags or treat all live bytes as durable.

### 3. Reopen, rather than overstate, the audit

Record the confirmed main-journal finding as `repair-required` until both
cache-only rollback and database-modified mixed-cohort playback preserve the
native mechanism. Keep the prior audit revision and historical results
explicitly scoped. Other mechanisms and later DAG-node states are unchanged.

The follow-up must transport the canonical synchronized-record boundary to
the existing Pager target before removing publication from database-modified
rollback. Its API, ownership and failure matrix require a separate accepted
design. No speculative field or unused extension is introduced in this first
slice.

## Test-first and qualification matrix

First add one focused real-backend regression: capture an original page,
arm the next journal-sync failure, and roll back without any database
authorization. The old implementation must fail by consuming that failure.
The corrected implementation must restore the exact image with zero journal
syncs and without header-publication writes.

Before sealing the slice, cover:

- generic coordinator cache-only rollback without a backend sync;
- initial unpublished and later unpublished cohort playback;
- published-header magic remaining mandatory;
- hot recovery of an unpublished first header remaining non-hot;
- hot recovery stopping before an unpublished later cohort;
- safe append and sequential-device paths;
- cache-only Pager rollback with unchanged bytes, zero database write,
  truncate and sync calls, and zero journal syncs;
- unchanged database-modified spill rollback and full-image restoration;
- exact metadata-read failure, malformed/checksum/crash tails, typed OOM and
  cursor-before-finalization lifetime controls; and
- journal/restore/finalization failure prefixes and second-crash recovery.

Retain all existing corpus records, assertions, allocation ceilings, stack
limits and deadlines. Behavior-specific expectations that describe the old
extra publication are superseded only by the explicit no-publication
contract; their original error and lifetime coverage stays represented.

Run the focused failure before implementation, focused green and complete
fast units afterward, then implementation review before committing.
Durable closing checks use the full change-aware qualification plan against
the final exact storage-repair head, including native interoperability,
fault/crash ordering, models, sanitizers and required hosted gates.
Repeat the same local fixed work after repair; do not turn one timing sample
or a green gate into a module-wide equivalence claim.

## Delivery boundary

This is an independent main-based node-43 storage repair. Draft PR #269 and
its four-commit diagnostic limit remain untouched; this is not a fifth
commit on that branch. ADR numbers 0166 through 0168 remain reserved by that
published draft.

A dependent native record-boundary correction may share this storage pull
request as a separately designed, tested, reviewed and ledgered commit under
ADR-0106. Schema admission, compiler allocation and Python fixture repairs
are separate ordered work, not additions to the durability slice.
