# ADR-0101: Typed Read Allocation Boundaries

- Status: Accepted
- Date: 2026-10-17

## Context

Reference-audit B1 identifies expected allocation exceptions escaping
record-codec and read-cursor `Result` APIs. Recompiling the existing bounded
diagnostic against the exact post-PR #210 library reproduces all nine named
sites: field-error construction, record-vector allocation, owned TEXT and
BLOB materialization, table cursor implementation, index comparison metadata
and implementation, and both payload-copy APIs. These are allocation sites
within one typed-error finding, not nine independent engine defects.

The canonical contracts already provide:

- `Error::OutOfMemory()`, which is nonallocating and nonthrowing;
- the record codec's nonthrowing `MakeError` and explicit owning
  `EncodeRecord` allocation boundary;
- one borrowed record validator/cursor and one owned `DecodeRecord`;
- one fixed-depth `CursorCore`, move-only root/path pins, and typed Pager
  allocation propagation;
- one cell-payload walker with feasibility checks and position-preserving
  read errors;
- operation-local bounded comparison scratch with existing narrow catches;
- one allocation-failure executable per codec/B-tree module; and
- independent SQLite records, page fixtures, interoperability contracts,
  corruption controls, and fixed resource baselines.

The field accessor bypasses the codec's safe error constructor. Owned decode,
cursor Open, and payload copies omit allocation containment. Cursor diagnostic
construction is also unprotected. The page decoder used by cursor positioning
and payload reads takes owning strings before its error helpers run, so a
malformed-page diagnostic can throw across an otherwise borrowed read path.
That directly coupled diagnostic seam belongs to this correction.

The two public payload-copy bodies currently duplicate the same algorithm.
They must end with one canonical implementation, not acquire separate
allocation/error policies.

Pinned SQLite returns `SQLITE_NOMEM` for record materialization and cursor
scratch allocation, including `src/vdbeaux.c:5427-5431` and
`src/btree.c:2910-2915,6215-6218`. C++ allocation sizes need not reproduce
SQLite's allocator layout. The required alignment is typed failure, bounded
ownership, cleanup, and unchanged persistent/error-order mechanisms.

Begin after PR #210, main commit
`ceeb3e8006afad3322698a93ff7022e619b1f06e`, accepted tree
`61eff1a3246c60b086285f031ceba09e72759271`. Its nine first-head checks
passed, including the original unchanged sanitizer model in 7.27 seconds
under its ten-second deadline. Node 43 remains active.

## Decision

### 1. Contain only expected allocation failures at existing boundaries

Catch only `std::bad_alloc` and `std::length_error` for the scoped ownership
and diagnostic construction, returning `std::unexpected(Error::OutOfMemory())`.
Do not catch arbitrary exceptions, turn an error into success, or introduce
another result type, generic recovery wrapper, allocator, or error framework.

Reuse the codec's existing nonthrowing `MakeError` for out-of-range field
access. Wrap only the owned materialization portion of `DecodeRecord`:
vector reserve, field ownership, and insertion. Keep validation before that
boundary and allow ordinary RAII to release partial vectors and values.

Value-returning ownership primitives such as `ByteBuffer`, `SqlValue::Clone`,
and `RecordFieldView::ToOwned` retain their existing exception policy. Their
explicit owning `Result` caller contains expected failures. Optimizer and
lowering propagation policies are not changed.

### 2. Make scoped diagnostic construction nonthrowing

Keep one existing module-local diagnostic mechanism in each scoped file.
Extend cursor error construction to the same narrow nonthrowing pattern
already used by the codec.

Page error helpers borrow `std::string_view` and perform message ownership
inside one protected local constructor. Remove eager string construction
before that boundary. This covers misuse, corruption, and range diagnostics
without changing successful borrowed decoding or introducing I/O.

Move every page-local `Result` error into `std::unexpected`; do not copy its
owning message after the protected factory. The current 15 forwarding sites
include `const` local Results, so remove that constness where needed before
moving the error. `std::move` of a const `Error` would still copy. Keep this
change local to the page decoder rather than changing `Error` globally.

When memory is available, preserve existing primary errors and diagnostic
text. If owning that diagnostic fails, return typed OOM. Do not change which
input is validated or its normal error precedence.

### 3. Preserve cursor ownership and state under failure

Public table/index Open are explicit allocation boundaries, including their
lower validation calls, copied index metadata, and implementation ownership.
Their local objects must unwind before OOM is returned. A failed Open leaves
no cursor or pin behind, and the same Pager remains usable.

Both payload-copy operations retain:

1. moved-from and current-position validation;
2. complete-payload feasibility before the payload allocation;
3. one exact-size `ByteBuffer`;
4. one existing full-range payload read; and
5. owned success or the existing typed read error.

Contain allocation failures without clearing an existing cursor position or
invalidating its borrowed local payload. Temporary copied bytes are released
on every failure. Do not retain another buffer, page list, or overflow cache.

After the focused fix and fast tier are green, consolidate the identical
payload-copy bodies into one `CursorCore` operation while keeping them green.
Both public cursor types delegate to it. The shared operation uses the same
walker and feasibility rule; no second reader or error policy remains.

Do not change seeks, traversal order, path depth, root retention, index-prefix
comparison, required overflow traversal, or handling of the unused final
overflow link. B2 remains a separate writable-reader correction.

### 4. Keep production scope bounded

Production changes belong only to:

- `src/format/record_codec.cpp`;
- `src/storage/btree/page.cpp`; and
- `src/storage/btree/cursor.cpp`.

No public API shape, persistent byte format, writer, Pager, VFS, session,
parser, binder, optimizer, lowering, VM, or later-node behavior is added.
The pure format/page modules perform no filesystem I/O.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before production changes. First add one
focused existing-helper field-error test and observe its expected allocation
escape. Then observe the expanded owned decode/Open/copy/diagnostic reds
before implementing their boundaries.

Extend the existing codec and B-tree allocation executables, not a second
failure injector. Retain the 1,024 allocation-disabled codec comparisons,
the current prefix-comparison cases and repetitions, and every old allocation
ceiling, cache budget, fixture, and workload.

Cover:

- allocation-disabled valid borrowed record/page access;
- field misuse/range and malformed-page diagnostics with normal errors when
  memory is available and typed OOM when message ownership fails;
- nested page/cell/overflow/freelist/pointer-map error propagation with every
  observed diagnostic allocation index, including indices after zero; require
  only one message ownership allocation and no later copy, while retaining
  the allocation-enabled primary code, text, and validation order;
- every vector/TEXT/BLOB decode allocation, independent returned ownership,
  cleanup of partial results, unchanged input, and successful retry;
- each warmed table/index Open ownership allocation, exact failure indices,
  complete pin release demonstrated by `EndRead`, and successful reuse;
- local and overflow copies for both cursor types, unchanged position and
  borrowed bytes after OOM, exact owned results on retry, and no live
  allocation leak; and
- existing source-I/O, malformed-input, impossible-payload, consumed-link,
  unused-final-link, and snapshot/lifetime controls.

Use the current nine-site diagnostic only as pre-change reproduction; its
success criterion intentionally expects the old throws and is not a green
post-change gate. Permanent tests require typed OOM.

Run focused and complete fast tiers after the fix and after the canonical
copy consolidation. Preserve independent record/page golden vectors and
interoperability in the unchanged native contract tier. Replay all 2,511
frozen cross-feature instances against the post-R1 results; normal native
and Modern outcomes must remain unchanged.

Run required compiler/sanitizer/static gates, 68 isolated native contracts,
27 inherited work properties, and appropriate bounded read-SQL/database
grammar campaigns. The matched performance baseline is PR #210's unchanged
native contract tier; no speed claim, instrumentation, threshold, or deadline
change is admitted.

Require GPT-5.6 Sol implementation and exact-tree review, unchanged-input
qualification, and all nine exact-new-head hosted checks before normal
squash merge. Verify merged main before cleaning only the confirmed merged
branch. Other findings, advanced-SQL slices, and node 43 remain open.

## References

- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0011: Typed Errors and Results
- ADR-0018: SQLite Record Codec
- ADR-0023: B-Tree Page Decoding
- ADR-0024: Read-Only B-Tree Cursors
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0088: Bounded Index Record-Prefix Comparison
- [Implemented-engine reference audit](../sqlite-reference-audit.md), B1
