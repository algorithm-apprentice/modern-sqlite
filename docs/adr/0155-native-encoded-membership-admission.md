# ADR-0155: Native Encoded Membership Admission

- Status: Accepted
- Date: 2026-10-26
- Module graph node: `implement-advanced-sql`
- Related repair: ADR-0154
- Amends: ADR-0117 keep-existing dispatch

## Context and completed source mapping

ADR-0154 removes the redundant search from replace-existing insertion.
Its focused sanitizer regressions pass, but the original local aggregate
deadline still fails. The fixture has 4,096 rows, 4,096 distinct categories
and two groups, so ordinary DISTINCT membership also encounters a missing
key on every accepted row.

Keep-existing insertion still performs `ContainsEncoded` followed by
`InsertEncoded`, opening two operation-scoped cursors and searching twice.
This does not reproduce the pinned SQLite membership admission mechanism:

- `select.c:989-997`, `codeDistinct`, emits `OP_Found`, then `OP_IdxInsert`
  with `OPFLAG_USESEEKRESULT`.
- `vdbe.c:5506-5568`, `OP_Found`, searches once and stores `seekResult`;
  an existing key branches without invoking insertion.
- `vdbe.c:6728-6753` passes that result to `sqlite3BtreeInsert`.
- `btree.c:9441-9603` saves other cursors and reuses the existing nonzero
  positioning result for a missing key rather than searching again.

The native sequence therefore probes first, returns immediately for a
duplicate, and saves peers only when a missing key will be inserted.
The positioned cursor must not move between those phases.

Modern already has every required mechanism. `SeekIndexRecord` returns a
value-owned `IndexSeekResult`; `CursorState::SavePeers` invalidates its
overflow cache and saves only other cursors; `BtreeDatabase::SaveCursors`
explicitly excludes the mutation cursor and performs no page mutation.
The original insertion position remains usable by `InsertIndexAt`.

A separate filesystem diagnostic also bounds local timing interpretation:
the same 448,552,504-byte sanitizer executable, authenticated by SHA-256,
takes 11.777 seconds from the NTFS checkout and 9.454 seconds from an owned
native Linux temporary directory, with the same SQL, source fixture,
sanitizers and ASLR. Observed major faults are 1,010 versus zero.
Elapsed time is captured before hashing. An earlier diagnostic incorrectly
included post-run hashing in wall time and is explicitly superseded.
Neither direct diagnostic is original CTest qualification, and no test
command, deadline, source path or build configuration is changed.

## Decision

Expose `Result<bool> IndexBtreeWriter::InsertIfAbsentEncoded(ByteView)`.
Successful `true` means inserted; successful `false` means the existing
complete representative was retained.

Extend the one internal encoded mutation primitive with `KeepExisting`.
Fully validate the encoded record and metadata, search once, and return
`false` immediately for an equivalent prefix. Do not save peers, promote
pages, or call the mutation primitive on that duplicate path.

For a missing prefix, save peers and pass the value-owned search result
directly to the existing `InsertIndexAt` insert-only path. Preserve its
formatting, overflow, balancing, rollback checkpoint and cleanup order.
Other insertion modes retain their existing save-peers-before-search order.
All expected failures remain typed results.

Share admission with ADR-0154's private writer helper using the existing
internal insertion-mode type, forward-declared for the private helper in
the public writer header. Do not invent another policy enum or extend the
table writer's public modes.

Both ordinary relation insertion modes use their single-call outcomes.
Only successful insertion increments the count; a duplicate retains the
old representative and a replacement retains the new one. The saturated
count path remains unchanged and rejects a missing key before mutation.

This completes the same bounded encoded-admission repair. It adds no
retained cursor, cache, tree, comparator, workspace, container, record
format, SQL behavior or cross-operation seek hint.

## Verification

Extend the existing comparison-callback regression before implementation:
keep-existing duplicate admission searches once and retains the original
record; missing-key admission also searches once, in both temporary modes.
The current missing-key path must fail with two callbacks.

Add direct writer outcomes and a positioned peer control: duplicate
admission must not move or save a peer cursor. Preserve every existing
OOM operation and inject every observed allocation boundary of the new
membership path, including root creation, overflow and peer-save failures.
The low-level Pager fixture retires its own read-only peer before rollback:
a failed peer-position capture may legitimately retain that peer's pin.
Require zero remaining pins and the same byte-exact rollback assertion;
do not change production cursor-trip policy or skip the failing prefix.
Existing malformed, numeric, NULL, collation, pin, poison, rollback,
interoperability and crash cases remain controls.

The focused regression, complete fast unit tier, independent source review
and final exact-head native/quality/sanitizer/fuzz gates remain mandatory.
Keep the original ten-second cases, fifteen-minute sanitizer job cap,
workloads, allocation bounds and assertions. No diagnostic or local
filesystem observation is a waiver or a substitute for hosted acceptance.
