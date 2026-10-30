# ADR-0173: Native Savepoint Cache Replay

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: ADR-0170 and qualified PR #271

ADR-0174 owns the subsequent physical-extent/shared-scratch and logical-only
savepoint follow-up. This ADR's bounded completion remains cache replay,
publication and writeback readiness, not whole-storage equivalence.

## Completed inventory and bounded finding

PR #270 delivers live full-rollback publication and per-record durability.
PR #271 delivers non-reader and transaction-command admission as qualified
main `7d6ba84`, tree `52a785fc7e074aa3d668bb7571d17a68cf11396f`. Neither
closes savepoint replay. The ordinary data-admission correction must not
inherit an unnecessary journal publication when closing a stale statement.

Both `Pager::RollbackToSavepoint` and `JournalTransaction::RollbackToSavepoint`
currently synchronize a dirty main journal before playback. The Pager target
also marks every cached replay image dirty, makes it clean after a database
write, and copies cached bytes before attempting that write.

Two actual ordinary nested-savepoint controls use `BEGIN`, prior writes,
`SAVEPOINT`, additional writes, measured `ROLLBACK TO`, release and outer
commit. All four native MSVC Release engine processes match 4096-byte pages,
a 512-page cache, DELETE/FULL, no mmap, memory temporary storage and normal
locking. The canonical `PinnedSqliteOracle` validates its complete profile,
source hashes and MSVC compile options, then independently verifies exact
final IDs/payloads, integrity, two pages, 8192 physical bytes and no sidecars.
Only hash-verified scratch files are removed.

| Measured rollback | Native | Modern |
|---|---:|---:|
| Cache-only main-journal writes / syncs | 0 / 0 | 1 / 2 |
| Spilled main-file writes / syncs | 2 / 0 | 2 / 0 |
| Spilled main-file truncates | 0 | 1 |

The cache-only result independently confirms the earlier transaction-owning
SAVEPOINT control. The new controls use an ordinary nested savepoint, not
native's null-savepoint transaction rollback mode. These are fixed-work and
state observations, not timing or complete I/O-parity claims. Physical resize
remains a separate correction: native retains spilled growth until committed
cleanup, whereas Modern shrinks it during replay.

### Completed pinned source map

| Native owner or operation | SQLite 3.54.0 source | Required translation |
|---|---|---|
| B-tree cursor fence before playback | `btree.c:4648-4675` | Retain ADR-0134's canonical save-all boundary. |
| Savepoint dispatch and retained target | `pager.c:7083-7152` | Replay without publishing the main journal or syncing the database. |
| Main then statement records; duplicate suppression | `pager.c:3425-3558` | Reuse the current cursor and shared PageBitvec driver. |
| Replay readiness | `pager.c:3460-3484` | Restore logical size and reset change-counter readiness without replacing the writer state. |
| Main-record database eligibility | `pager.c:2459-2475` | Use ADR-0170's synchronized-end proof, not global journal dirtiness. |
| Statement-record eligibility | `pager.c:2461-2465` | Use cached `NEED_SYNC`, or the absent-page case, with database-modified state. |
| Database write precedes cached replacement | `pager.c:2467-2538` | Stop on failed database write before copying cached bytes. |
| Missing statement image and no database write | `pager.c:2490-2513` | No-content cache fill with all spill inhibited, then make the new frame dirty. |
| Existing cached replacement | `pager.c:2514-2538` | Preserve dirty, sync and DONT_WRITE flags; do not make a replayed page clean. |
| No-content fetch and dirty transition | `pager.c:5597-5705`; `pcache.c:593-606` | Reuse cache ownership and fill; do not infer a NEED_SYNC assignment from an older explanatory comment. |
| First spill | `pager.c:4663-4735` | Sync when the page needs it or the writer is still cache-modified. |
| Successful writeback publication | `pager.c:4340-4450,4481-4558` | Clear sync flags and enter database-writeback readiness before filtering DONT_WRITE pages. |
| Physical size and committed cleanup | `pager.c:2726-2759,2194-2202,6717-6733` | Separate follow-up; this ADR does not change those transitions. |

Native no-content fetch also publishes benign page-membership hints. Modern
already captures and retains its required membership through the canonical
journal owner. Its page permutation requires equal source and target sets;
it is not native's one-way Movepage with an original-page sync ghost
(`pager.c:7280-7362`). Do not introduce a movement API or a second membership
algorithm merely to manufacture that native caller.

### Existing canonical Modern mechanisms

- `RollbackSavepointPlayback` already transports explicit main-record
  durability and leaves statement images unmarked. Its borrowed record
  buffer and ordered completion lifetime remain authoritative.
- `ApplyPlayback` validates images, filters the original-size domain and
  suppresses duplicates with `PageBitvec`. ADR-0045's accepted empty-domain
  representation remains unchanged.
- `Pager::RecoveryTarget` is the sole cache/database replay target. Full
  rollback and hot recovery retain their existing cache invalidation,
  database synchronization and finalization.
- `PageCache` owns stable frames and private prepare/publish fill. Its soft
  capacity can allocate a frame without calling Pager spill. Header refresh
  already uses this no-spill path.
- Public `Pin::mutable_bytes` requires dirty, writable state. `MarkDirty`
  clears DONT_WRITE and `MarkClean` clears sync flags. Neither is a
  flag-preserving cached replay primitive.
- Existing private cache operations validate Pager pin provenance and
  exclusive ownership. Add the missing byte replacement at that same narrow
  boundary, not a public mutable capability or another cache.

## Decision

### 1. Restore cache bytes without changing existing frame flags

Add one private `PageCache::RestoreBytesForPager(Pin&, ByteView)` operation.
It requires an exclusive pin owned by that cache and exactly one page of
bytes. Invalid provenance, ownership or size returns the existing typed
misuse result. Valid replacement allocates nothing and changes no dirty,
sync, DONT_WRITE, pin, hash or ownership state. Keep public mutable-byte
guards unchanged. Reuse the existing test peer for its owning tests.

In the savepoint target, look up the existing exclusive pin and derive
database eligibility before changing its bytes. Write an eligible image to
the database first. Return a write error without copying that cached image.
Only after success replace existing cached bytes through the private
operation, preserving their flags even when the image was also written.

If a statement image is absent and was not written, use the existing private
prepare/publish cache fill without Pager pressure or database read. Install
the complete borrowed image and mark the new frame dirty. Do not invent
NEED_SYNC or call ordinary writable-page admission, which would capture
another journal image. Existing journal membership and byte ownership stay
with their canonical owners. Main-image cache misses do not require this
statement-image refill.

Preserve the current writer state through savepoint replay. Reset the
existing change-counter readiness when restoring the savepoint image,
rather than infer it only from a restored page-1 record. Remove the now
redundant private restored-page-1 flag. Keep existing cursor fences, raw-pin
rejection, write-generation invalidation, retained savepoint membership,
logical-size validation and typed failed-playback/full-rollback ownership.

Use the native first-spill condition: a persistent dirty page requires the
existing sync boundary when its own flag requires it or the Pager remains
`kWriterCacheModified`. Reuse `SyncJournalForWriteback`; no second
publication or write-authorization algorithm is added.

### 2. Remove both conservative savepoint publication owners

After the cache-replay slice is reviewed and sealed, remove the dirty-journal
pre-sync from both Pager and JournalTransaction savepoint rollback. A
cache-only rollback must neither upgrade the database lock for publication
nor consume an armed journal-sync failure.

Do not clear `main_journal_dirty`, advance synchronized-end metadata or claim
that a replay made the main journal durable. Existing per-image proof and
cached sync flags decide which originals may reach the database. Preserve
commit, spill and hot-recovery synchronization, ordinary/live record
validation, no-savepoint duplicate rules, cursor destruction before backend
completion, explicit errors and retryable full-rollback cleanup.

### 3. Keep the remaining boundaries explicit

This ADR does not defer physical savepoint truncation or redesign database
extent tracking. That difference remains demonstrated and repair-required.
It does not change SQL admission, schema retry, transaction error precedence,
compiler ownership, WAL, concurrency or any later DAG node.

No public API, second image representation, allocator policy, scratch byte
vector, backend plugin or trusted verifier bypass is introduced.

## Ordered test-first delivery

Use two related coherent commits under ADR-0106:

1. Correct cache replay, readiness and first-spill behavior while retaining
   both pre-sync owners. Begin with a real Pager regression whose replayed
   cached frame must remain dirty after an eligible database restore.
   Record its runtime failure on the accepted predecessor. Cover private
   byte replacement's clean/dirty/sync/DONT_WRITE combinations, foreign,
   moved and nonexclusive pins, exact/invalid sizes and allocation-free
   valid work; cached database-write failure ordering; writer-locked
   no-change rollback; change-counter retry; existing fill ownership and
   allocation failure; and reachable Pager/B-tree savepoint lifecycles.
2. Remove both pre-sync owners. Begin with a cache-only real Pager rollback
   and an armed journal-sync failure; it must retain the failure rather than
   publish the journal. Cover mixed durable/unsynchronized main cohorts,
   statement images, repeated/nested rollback, page 1, ordinary and
   safe-append devices, commit/hot negative controls and rollback fault
   prefixes. Replace the old explicit two-sync expectation with the
   source-proven native contract, retaining its data and lifecycle checks.

Do not add an impossible one-way permutation or weaken pin rules to hit a
refill branch. Keep the canonical fill's own boundary/OOM tests and state
which replay branches a reachable owning scenario actually executes.

Each commit requires focused green, complete fast units, independent
implementation review and append-only evidence before its successor. At the
final exact batch head run change-aware isolated native/sanitizer,
interoperability, model, crash, production analysis, discipline, format and
required hosted gates. Replay the two matched public controls unchanged and
independently verify every final database. No workload, assertion, allocation
ceiling, deadline, native stack or compiler/sanitizer policy is relaxed.
Physical-resize and broader mechanism closure are not inferred from passing
this bounded cache/publication correction.

## Native quality portability follow-up

The first slice's complete native 2,166-case fast tier and genuine GNU13
ASan/UBSan 2,150-case unit/compatibility/model selection pass. Full native
analysis then reports two const-correctness diagnostics on unchanged
`AcquireFillSlot` and `AllocateEntry` locals. Exporting both the original main
`7d6ba84` source and its original owning cache header reproduces both
diagnostics with the same compiler arguments and checks.

Use the existing free-list owner directly for the nonempty check, and return
the newly published entry through the canonical `entries_` owner rather than
retaining unnecessary mutable local pointers. Slot order, frame addresses,
allocation volume, exception cleanup and returned mutable capability remain
unchanged. This is a localized green-baseline refactor, not a new cache
algorithm or a const-cast, warning suppression or global policy exception.

Re-run full native analysis, focused tests and complete fast tiers against
the new exact source, then obtain independent implementation acceptance.
The prior exact-tree source verdict and sanitizer report remain historical
green-baseline evidence, not final evidence for the changed source.

## Completed writeback-readiness prerequisite

The first published source `bb246d2` passes native correctness and sanitizer
gates, but its intermediate hosted diagnostic contracts expose a missed
paired state transition: both Modern DELETE-scan profiles perform 2,046
journal syncs against the unchanged 32-call ceiling. The two original
contracts are independently reproduced with the canonical GNU13 Release
targets and unchanged fixtures/workloads. This is a fixed-work regression,
not a timeout or permission to weaken the ceiling.

The complete native caller chain is `pagerStress` ->
`syncJournal` -> `pager_write_pagelist`. Successful `syncJournal` clears page
sync flags **and publishes `PAGER_WRITER_DBMOD` before any database write**.
The page-list writer requires that state even when every page is skipped by
DONT_WRITE. Modern's helper clears flags but omits the promotion; its
DONT_WRITE path returns before the later successful-write assignment.
Adding the native first-spill predicate without its paired promotion can
therefore republish subsequent tails for already-protected skipped pages.

Complete this prerequisite in the second coherent source. After successful
`SyncJournalForWriteback`, publish `kWriterDatabaseModified` after flag
clearing, independently of whether that attempt writes a page. Failures
return before promotion. Preserve the native first-spill predicate.

Use this canonical writer phase for full/savepoint recovery eligibility.
Remove the redundant private `database_bytes_modified_` signal and its
assignments rather than retain two conflicting readiness definitions or add
a journal-ever-synced heuristic. The only remaining helper callers are
commit and persistent spill; hot recovery continues supplying its explicit
target. Nonwriter, writer-locked, ephemeral, terminal-cleanup and failed
write behavior retain their existing boundaries. Physical resize remains
the separately recorded gap.

First add a real DONT_WRITE-spill runtime red: successful journal publication
with zero database writes must still enter native writeback readiness. A
subsequent protected DONT_WRITE page beside an unsynchronized new tail must
not republish that tail, and full rollback must restore the eligible prefix.
Keep both original DELETE-scan contracts and their 32-sync ceiling, and
requalify the complete dependent publication source and final batch.
Const-qualify the two copied-result locals identified by actual macOS
analysis; no diagnostic suppression or compiler-policy change is admitted.
The first normal commit is not amended, and its failed CI remains evidence.
