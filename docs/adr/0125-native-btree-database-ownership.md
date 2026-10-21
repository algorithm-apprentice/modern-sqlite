# ADR-0125: Native B-Tree Database Ownership

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, shared B-tree prerequisite

ADR-0127 adds the native-shaped physical cursor list and current phase
ownership to this database owner. Actual save/reseek remains its following
behavioral mechanism.

ADR-0134 places savepoint playback behind that owner's actual global cursor
save/release fence. Pager-only operations still borrow the existing Pager;
this does not introduce a complete transaction facade.

## Native mechanism and missing owner

PR #234, main `b1fcafa`, closes the register-ownership prefix, not shared
B-tree cursor coordination. The unchanged four-range control records one
native cache miss versus 24 Modern misses. However, adding `aOverflow`
without its native owner and write coordination would be an incomplete
translation.

The proposed PageCache-wide write-access generation was withdrawn before
production. It invalidated unrelated cursors during cache work such as dirty
spill, while retaining pins that native `saveAllCursors()` releases.
Memory safety or equal SQL output alone does not justify that replacement.

| Responsibility | Pinned SQLite source |
|---|---|
| Connection handle and shared Pager/cursor owner | `btreeInt.h:345-358,425-429` |
| Allocate B-tree ownership before opening Pager | `btree.c:2562-2716` |
| Close requires closed cursors, then releases Pager | `btree.c:2951-3007` |
| Same-root pre-mutation cursor saving | `btree.c:759-856,9470-9472,9961-9965` |
| Saved-key restore and skip state | `btree.c:865-1007` |
| Cursor-owned overflow page list | `btree.c:5155-5355` |

Modern read cursors accept a bare Pager, while `BtreeWriterCore` is owned by
a write-transaction session. Neither is the native database-lifetime owner
shared by all read and write cursors. The writer's root-incarnation and
statement epochs do not replace that owner.

## Decision: establish the canonical owner first

Introduce one `BtreeDatabase` in the B-tree layer. It uniquely owns one Pager,
corresponding to the single-connection `Btree`/`BtShared` lifetime. Do not put
B-tree state in PageCache/Pager, introduce a global owner lookup, or use an
opaque attachment, callback registry or compatibility wrapper to hide an
upward dependency.

The database object is noncopyable and nonmovable; callers move its
`std::unique_ptr`. Its address and Pager address remain stable. The VFS
outlives the database, and every cursor/pin is closed before its owning
database is destroyed, matching the native close precondition.

Provide typed read-only, writable, ephemeral-file and ephemeral-memory open
factories matching the existing Pager configurations. Allocate the B-tree
owner before opening its Pager and unwind both explicitly on failure.
Use existing construction-key, typed-result and RAII patterns. There is one
owner allocation per database open, not per cursor, statement or row.
The construction key is not externally constructible, including through an
empty braced argument; only a successful factory publishes the database.

Read/write cursor and `BtreeWriteSession` entry points take this owner,
not a bare Pager. Catalog loading and VM execution attachment use the same
owner. Read/write sessions, the transaction coordinator and temporary B-tree
roots retain it. Existing Pager-only work borrows `database.pager()`; do not
forward the entire Pager API through a second facade.

Update all directly dependent call sites together. Do not retain old
Pager-taking overloads that silently create independent owners. VM preparation
remains storage-independent, and public SQL session behavior is unchanged.
Existing root validation, writer generation, statement capability, byte
ownership, error and cleanup contracts remain in their current canonical
implementations.

This supersedes only the bare-Pager ownership seams in ADR-0024, ADR-0030,
ADR-0032, ADR-0044 and ADR-0047. Their traversal, catalog, execution and
transaction semantics are unchanged.

## Delivery and qualification

This is one coherent ownership refactor, starting from the green,
source-equivalent PR #234 baseline. Add owner lifetime, open failure and
pointer-stability tests before implementation; preserve existing normal,
malformed, allocation, I/O, snapshot, rollback and temporary-storage tests.
Do not manufacture a behavioral failure for unchanged SQL semantics.

Review this design before production. Parent owns implementation and routine
gates; GPT-5.6 Sol reviews design, code and exact-head integration. A complete
default build precedes the focused controls and complete fast unit tier.
Run the actual-change closing plan on the exact final source, preserving
native/interoperability, genuine GNU, sanitizers, four fuzz campaigns,
matched workloads, allocation limits and original model deadlines.
Publish the complete ledger and require all nine latest hosted successes
before guarded squash, accepted-tree proof and merged-branch cleanup.

## Explicit follow-up boundary

The new owner is the prerequisite, not a claim that cursor behavior already
matches native. Next translate one root-filtered cursor list and the actual
pre-write protocol: save rowid or encoded key, preserve possible I/O/OOM
failure before mutation, release page stacks, enter `REQUIRESEEK`, restore
once with native skip/deleted-row handling, then add the native overflow list.
No substitute global generation is admitted.

This refactor introduces no unused cache APIs, new SQL, shared-cache mode,
concurrency policy, persistent format or second executor. Native Column,
large-value sharing, broader payload shortcuts and remaining ledger-12
attribution stay open. Later ledgers and DAG nodes remain blocked.
