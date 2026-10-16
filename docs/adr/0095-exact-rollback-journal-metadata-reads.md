# ADR-0095: Exact Rollback-Journal Metadata Reads

- Status: Accepted
- Date: 2026-10-16

## Context

The implemented-engine reference audit identified a durability defect at the
VFS/journal recovery seam. Modern's VFS deliberately returns a successful
short read with a zero-filled suffix. Rollback playback currently converts
an incomplete header or super-journal metadata read into an unusable/torn
journal, even when the preceding file-size check says all requested bytes
exist. Hot recovery can then report success and delete the authoritative
journal without restoring the database.

Pinned SQLite distinguishes these cases:

- `src/os_unix.c:3508-3578` reports `SQLITE_IOERR_SHORT_READ` after zero
  filling;
- `src/pager.c:1627-1674` accepts a physically absent journal header as
  end-of-journal, but propagates a read error within a size-admitted header;
- `src/pager.c:1338-1385` propagates super-journal trailer/name read errors;
- `src/pager.c:2972-2995` tolerates a short page record as a crash tail; and
- `src/pager.c:3007-3039` finalizes the journal only after successful
  playback.

ADR-0041 already requires actual VFS failures to remain errors. This
correction supplies the missing interpretation of short-read results at
metadata boundaries. It does not change the VFS short-read contract or
invent a new recovery mechanism.

This decision explicitly supersedes ADR-0040's ambiguous "incomplete later
header" clause and ADR-0041's "short later header" clause. A physically
absent header sector is a tolerated tail; a short VFS read within a
size-admitted metadata region is not.

## Decision

### 1. Require complete metadata after structural bounds admission

Keep the existing size checks and structural validation. A file too short
to contain the next complete journal sector remains an ordinary crash tail.
A complete read with unpublished/bad magic, invalid geometry, or an invalid
super-journal trailer retains its existing native-compatible disposition.

After size and offset validation establish that the requested metadata is
present, require the whole read to complete. Apply one shared internal
helper to the first header, later headers, super-journal trailer tail, and
validated super-journal name.

An incomplete required read returns the typed extended
`SQLITE_IOERR_SHORT_READ` error (primary `10`, extended `522`) through the
existing error representation.
Preserve an underlying explicit VFS error unchanged. Contain allocation
failure while constructing a new diagnostic error and return the canonical
out-of-memory error; do not let error reporting add an exception path.

Do not apply this stricter helper to page-record crash-tail handling or
change the behavior of unrelated VFS consumers.

Retain the existing coalesced 28/20-byte header and 16-byte trailer reads.
"Validated super-journal name" means the current parser order, which checks
tail magic before requesting the name. Native checks length and then reads
the name before its final malformed-magic disposition. Exact syscall
granularity and every malformed-trailer/read-fault error-precedence
combination are not claimed equivalent by this correction; that bounded
pre-existing parser-order difference remains visible in the audit.

### 2. Preserve authoritative recovery state on failure

Use existing typed propagation through playback, `RecoverHotJournal`, and
Pager. A metadata read failure must not authorize journal deletion,
successful recovery synchronization, a recovered snapshot, or transaction
success.

Do not add a retry loop, secondary recovery reader, alternate journal owner,
or success-shaped fallback. Previously restored pages during a later-header
failure may remain partially restored, but the journal remains available and
the failing Pager does not publish a usable snapshot. A new recovery owner
can replay the retained journal after the injected transient failure is gone.

Keep journal publication, database write/sync ordering, checksums, physical
geometry, savepoint boundaries, and cleanup ownership unchanged.

## Verification

Review the design before production changes. Observe a focused regression
against the unchanged implementation:

1. create a published hot journal for a partially modified database;
2. keep the journal's reported size unchanged;
3. allow the hot-journal probe but short-read the first recovery header;
4. require an I/O error, no published snapshot, and the retained journal; and
5. remove the one-shot fault and recover the old image with a new owner.

Cover first and later headers, super-journal tail/name reads, explicit VFS
errors, and a later-header failure after earlier pages were restored.
Retain independent controls for physically truncated headers/records,
unpublished magic, malformed trailers, safe-append journals, ordinary
transaction/savepoint rollback, and existing crash-cut recovery.

Use the existing deterministic VFS failure seams. Run focused journal/Pager
tests, the complete fast tier, unchanged native interoperability/crash and
performance contracts, compiler/sanitizer/static gates, and exact-tree review.
Do not alter fixture bytes, crash-cut ranges, allocation budgets, or deadlines.

### Design and red evidence

GPT-5.6 Sol accepted this bounded design before production changes.
The first Pager regression observed false recovery success, journal removal,
snapshot publication, and failure to restore the old page after a short
header read. The expanded first/later-header and super-journal tests also
failed, while the short page-record control passed. The recorded expected
red was three failures and one passing control.

### Local closure evidence

The correction passes 1,403 fast unit cases. Complete Debug, Release, and
ASan/UBSan tiers each pass 1,453 cases; instrumentation passes 1,465, and
GNU 16 Debug/Release each pass 1,450. The complete matrix was followed by
an equivalent constant-spelling lint correction and canonical `ByteBuffer`
ownership for one test snapshot. All 110 focused journal/Pager/allocation
cases then passed on each of the six configurations, with the complete fast
tier and 68 isolated native contracts repeated successfully.

The new allocation regression rejects the frozen pre-correction library
and passes with the correction, including every measured allocation index
on the new diagnostic-error path. The 27 inherited source-correlation
fixed-work properties remain identical.

The ASan/UBSan grammar-aware read and write campaigns each completed
20,000 inputs with seed 91. Repository corpus seeds, fixtures, workload
definitions, native profiles, crash ranges, allocation budgets, and
timeouts are unchanged. Formatting, changed-translation-unit clang-tidy,
engineering discipline, dependency-graph validation, and whitespace checks
pass.

GPT-5.6 Sol approved the bounded implementation review. Final exact-tree
acceptance and exact-head hosted gates remain publication requirements;
local success is not evidence that the separate hosted model deadline has
been repaired.

### Integration with the completed deadline correction

PR #204's initial journal head was held because the unchanged correlated
model still timed out at 10.02 seconds. ADR-0096/PR #205 corrected that
separate algorithmic cause and merged as
`75308c5160689864c6855f4715963b30bdb16acb`. Its first exact-head hosted
sanitizer run passed the original model in 6.60 seconds under the unchanged
ten-second limit, with all nine checks passing.

Refresh this journal slice through a normal merge of that main revision,
preserving its published history. The journal and optimizer source hashes
remain identical to their separately accepted implementations. Reconcile
the ADR index/DAG and audit status, then require combined-tree review and
new exact-head validation before merging the journal correction.

## Traceability

Node: `implement-advanced-sql`, prerequisite durability correction.

Contracts: ADR-0002, ADR-0004, ADR-0011, ADR-0019, ADR-0040, ADR-0041,
ADR-0042, and ADR-0045.

The separately reviewed NULL-range correction and the remaining audit
findings remain separate. This correction does not complete node 43 or
implement the algorithmic deadline correction delivered separately by
ADR-0096.
