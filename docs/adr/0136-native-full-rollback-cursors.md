# ADR-0136: Native Full-Rollback Cursor Coordination

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Mapped and executed native behavior

ADRs 0134/0135 provide savepoint saving and native trip selection. Full
rollback still reaches Pager directly, so retained cursor paths make it
return Busy. Native always coordinates cursors before playback, and does not
use the same trip policy for every caller.

| Responsibility | Pinned SQLite source |
|---|---|
| Global save or trip, save-error escalation, playback despite save failure | `btree.c:4552-4596` |
| Pager error overrides the earlier save error | `btree.c:4572-4580` |
| Reader/open no-op and persistent Pager error | `pager.c:6830-6881` |
| Explicit SQL rollback uses abort-rollback policy | `vdbe.c:4150-4176` |
| Schema-wide versus write-only abort | `main.c:1516-1552` |
| Special errors, compiler journal flag, ordinary implicit abort and failed commit | `vdbeaux.c:3326-3486` |
| Physical key capture from held pages, independent of public movement | `btree.c:713-791` |

Six executed low-level native controls cover preserve-all, abort-write-only,
abort-all, save OOM in both saving modes, and Pager-error precedence. Every
path is released. Save OOM faults every cursor with code 7 but still rolls
the transaction back. The precedence control deterministically seeds a
terminal native Pager state and observes return code 10 while cursors retain
their earlier code 7; it is a state fixture, not a naturally occurring
in-memory filesystem fault or evidence for Modern Pager recovery.

Six additional actual SQL controls compare ordinary, I/O and OOM function
errors in implicit INSERT/UPDATE. Native UPDATE has `usesStmtJournal=1`;
INSERT has zero. Ordinary implicit abort saves both read and write peers.
I/O trips the writer in either program. OOM trips it only when the compiler
journal flag is false. Retaining only an actual anonymous-savepoint ID
cannot reproduce this: implicit statements can have that flag without an
anonymous savepoint.

## Decision

### 1. Add the native cursor prefix to the existing rollback owner

Add only
`BtreeDatabase::PrepareRollbackCursors(std::optional<BtreeCursorTripMode>)`.
An absent mode means native `tripCode == OK`: save every cursor through the
existing root-zero protocol. A present WriteOnly/All mode invokes ADR-0135.
Reject an invalid present mode before moving any cursor. If global saving
fails, trip every cursor with that exact failure code and return the
original error.

The coordinator already owns Pager rollback and its retry/cleanup phases.
Keep that ownership rather than introduce a wrapper that masks and loses
the cursor outcome behind a later Pager error. It records this prefix's
outcome once, then still invokes the existing Pager rollback when its state
requires it, even after a cursor failure. Reader/open states require no
Pager rollback. A Pager error wins for that call; after Pager completes,
the retained cursor error still has to be returned.

This separates the same native cursor-before-playback ordering across the
existing acyclic owners; it does not create a second rollback state
machine. Preserve native partial-save/trip ordering and all key/path cleanup.
Keep the current Pager durability, terminal-error, ephemeral-mode, raw-pin
and EndRead contracts. No journal, decoder, traversal, callback, generation
or native other-active-reader admission from ADR-0119 is added.

### 2. Capture an existing physical position, not a new user read

Native saveCursorKey reads a held valid page even while rollback is cleaning
up a failed write. Current read capture calls the public snapshot-checked
cell/payload path; writable capture calls mutation-authority checks. Those
guards can reject cleanup before it captures an already-owned local key.

Separate the checked public entry points from their canonical held-position
work. Reuse one current-cell decoder and one payload-copy loop per existing
physical role. The private capture hook, called only for Valid/normalized
SkipNext by CursorState, reads its existing pinned path and validated
geometry without starting a user read or reviving a write capability.

The path pins keep local bytes alive; snapshot replacement cannot reclaim
them. Capture still validates cell/record geometry and allocation bounds.
Overflow acquisition uses the existing Pager and returns its real error.
Use one transient immutable Pager pin in the writable payload loop, matching
native payload acquisition, rather than requiring mutation authority merely
to read an overflow page. Retain consumed-link validation and every copy,
allocation and failure ordering contract. Do not add an alternate reader or
unchecked public API. Ordinary snapshot, generation and statement checks
remain unchanged.

### 3. Preserve the native caller's policy

Keep the existing logical rollback-scope decisions from ADR-0119. Retain the
complete `TransactionStatementOptions` in ActiveStatement instead of
separate access/rollback copies, so the compiler journal flag remains known
for implicit statements too.

| Existing full-rollback caller | Cursor policy |
|---|---|
| Explicit rollback or full abort of an explicit transaction | Abort, WriteOnly or All from the canonical schema flag |
| Ordinary implicit statement abort/cancellation | Save all |
| Implicit OOM/FULL with compiler statement-journal flag | Save all |
| Implicit OOM/FULL without that flag | Abort |
| I/O or writable interruption requiring full rollback | Abort |
| Failed implicit commit before its durable cleanup phase | Save all |
| Anonymous/named savepoint or setup failure escalating to full rollback | Native abort policy, with implicit setup using its known statement options/error class |

Read statements do not acquire a write statement-journal flag. Ordinary
read errors/read-only interruption still finish only their existing read
scope. No conflict-specific or multiple-active-statement SQL is added.

Make policy selection explicit at the current full-rollback entries.
Reuse one schema-to-trip selector for named and full rollback. Do not infer
policy from the presence of an actual anonymous savepoint.

### 4. Retain the outcome and progress of the existing operation

Group rollback-attempt metadata in the coordinator: the selected optional
trip mode and an optional completed cursor-fence `Status`. An absent outcome
means the prefix has not run; a stored success or error means it has run.
The existing TerminalPhase and authoritative Pager state retain progress.
Do not add another independent Pager-progress flag.

Run the cursor prefix at most once for that operation, including after a
failed fence. Retain its exact error even when a later Pager Busy/I/O error
wins the current return value. A retry neither changes an implicit preserve
policy into explicit abort nor overwrites earlier writer/read fault codes.
For example, save OOM followed by raw-pin Busy returns Busy first; after the
pin is released, successful playback/cleanup still returns the retained OOM.

RollbackAttempt delegates only the remaining Pager operation after the
prefix. Pager WriterFinished is already cleanup-only progress: invoke its
existing continuation, never repeat the fence or journal playback. Count
the successful Pager rollback once at its existing successful completion.
RollbackCleanup performs only retained read cleanup. It cannot call the
prefix or Pager rollback again.

Factor the existing read-unlock transition from its completed logical reset
so commit and rollback still share one cleanup implementation. On complete
rollback cleanup, move the retained fence outcome out before logical reset
destroys the attempt metadata, reset logical state, then return that
outcome. On failed cleanup, retain the attempt and its cause; augment errors
through the existing primary-error mechanism without consuming the stored
outcome. Do not copy away or silently discard a masked error.

Thus completed playback can legitimately accompany a returned error, but
the connection is not left looking active after completed cleanup.
Commit-cleanup exclusion, stale authority and existing retry preconditions
remain unchanged.

### 5. Retire completed caller operations without hiding their error

Apply ADR-0098's authoritative token-retirement rule to
`RollbackActiveStatement()` as well as successful-completion handling.
After Rollback returns, inspect the same TransactionStatement's `valid()`.
If it retired, discard the existing pending wrapper/publication state even
when the result is an error, and still propagate that original result.
A valid token retains its incomplete operation for retry. Never infer token
retirement from autocommit, error text, an error whitelist or Pager state.

Transaction SQL commands do not own such a token. Add only the nonallocating
`TransactionCoordinator::pending_completion()` projection of the existing
TerminalPhase: absent, Commit, or Rollback, through a descriptive typed
completion kind. This projection stores no new progress or ownership.

An Error-Step with pending Rollback invokes the coordinator's existing
Rollback continuation directly. It must not reissue SAVEPOINT, RELEASE or
ROLLBACK TO: those commands reject a rollback terminal phase and cannot
finish the automatic full rollback they initiated.

When that cleanup belongs to an originally failed command other than full
ROLLBACK, keep the existing session `last_error` authoritative. Do not clear
it before continuation, overwrite it with a later cleanup error, or report
Done merely because the fallback rollback finally completed. Return that
original error after successful cleanup; augment it with a current cleanup
failure without consuming the retained cause. This uses the existing error
and lifecycle, not another retry flag or command token.

For an explicit full ROLLBACK, successful continuation retains its existing
Done behavior. Pending Commit continues through the current valid
COMMIT/outer-RELEASE entry points; an unrelated failed command must not force
a commit. Once no completion remains, ordinary Reset/Finalize retire the
previous execution and preserve its error rather than retrying a completed
operation.

Keep the existing automatic fresh Step reexecution contract: once no
completion is pending, a subsequent automatic execution uses the normal
reset/catalog-refresh path, not the old cleanup retry path. A deliberately
fresh ROLLBACK against autocommit may still return SQLite's ordinary
no-transaction error; do not fabricate successful execution or change
prepared-statement reuse semantics.

This narrowly extends ADR-0098's no-new-API boundary because SQL transaction
commands have no TransactionStatement token. It introduces no completion
token, registry, per-session retry flag or reconstructed state machine.

## Test-first and closing boundary

First reproduce real full rollback returning Busy with positioned table/
index peers. Match all three native modes, deleted-key continuation,
write-only/schema fault codes, read-only and deferred transactions, complete
path release, and raw non-cursor pin rejection without hidden retries.

Cover held local-key capture after a write failure, malformed/overflow/OOM
save failure, all-cursor escalation, playback despite save failure and
Pager-over-save error precedence. Exercise implicit ordinary/OOM/FULL/I/O
policy with both compiler flags, explicit abort, failed commit, and the
existing anonymous/named fallback paths.

Prove completed-playback error/counter/autocommit behavior and both attempt
and cleanup retries without changed policy, extra journal playback or lost
stored cause. Combine save failure with raw-pin Busy, WriterFinished
shared-unlock failure and EndRead cleanup failure; final success must not
erase the saved error. Cover retired writable-statement Reset, Finalize,
reprepare/reexecution and SQL ROLLBACK's completed-versus-pending retry path
through the existing lifecycle helpers, without adding a public storage
escape hatch or a fake successful rollback.

Add the reachable public SAVEPOINT regression: after an explicit write,
fail Pager savepoint creation and the fallback rollback's shared-lock
downgrade. Repeated Step must continue the existing WriterFinished rollback,
not reissue SAVEPOINT, and must retain the original command failure after
cleanup rather than report Done. Retain the existing successful COMMIT and
outer-RELEASE cleanup retries and ordinary fresh execution behavior.

Preserve all allocation limits, workloads, compiler/model
deadlines, native/crash/interoperability and format contracts. Complete
focused red/green, freshly linked fast units, affected isolated/compiler/
quality gates, exact-tree review and committed native/matched evidence
before publishing this source's ledger.

Final exact-head batch gates, complete published ledger, integrated review
and all nine latest hosted successes remain mandatory. This is full
rollback coordination, not completion of schema destruction, overflow
lists, lazy Column, ledger 12, node 43 or later nodes.
