# ADR-0088: Bounded Index Record-Prefix Comparison

- Status: Accepted
- Date: 2026-10-15
- Module graph node: `implement-advanced-sql`

## Context

PR #198 completes stored joined-index ranges under ADR-0087. ADR-0053 next
requires measured automatic covering indexes for repeated unindexed
equality joins. Before that source can be integrated, design review found a
shared lower-layer resource prerequisite.

`IndexBtreeCursor::Seek()` and `CompareCurrent()` already implement canonical
prefix comparison. For overflow records their shared comparison helper
copies the complete record into a retained `std::vector<std::byte>`, even
when only the first field is compared. Covering payload fields can therefore
remain in a temporary relation's read cursor outside statement retained-byte
accounting. Reusing that implementation unchanged would not satisfy
ADR-0058.

Existing `ReadCellPayload()` can read a bounded byte range, but stopping at
the prefix would hide malformed overflow links or I/O failures that complete
comparison currently observes. Existing `RecordView` deliberately borrows a
complete encoded record. Pretending a truncated prefix is a complete view
would weaken its invariant and its cursor's bounds proof.

This prerequisite is one coherent, end-to-end storage/format slice. It
changes the existing persistent-index comparison mechanism used by all
public-session consumers, not a disconnected automatic-index prototype.
Automatic source selection, temporary lookup states, bytecode, VM source
borrowing, Bloom filtering, and any planner-option changes wait until this
slice is reviewed and merged.

The unchanged-production public diagnostic already records 20,504,
184,372, and 676,012 VM instructions for one, eight, and 32 equality probes
against the frozen 4,096-row unindexed payload source. This is the later
automatic-source baseline, not evidence of a prefix-comparison speedup.
No cross-engine timing claim or frozen benchmark change is part of this
prerequisite.

## Decision

### 1. Add a pure borrowed record-prefix view

Keep `RecordView`'s complete-record invariant unchanged. Add
`RecordPrefixView`, borrowing the complete encoded header and the contiguous
body bytes of a requested initial field prefix.

Its narrow format API provides:

- the required header size from an available initial byte span and declared
  complete record size;
- the required prefix byte count from a complete header, declared complete
  record size, and nonzero requested field count;
- parsing of those prefix bytes into a borrowed view;
- complete and available field counts; and
- a `RecordCursor` limited to the available prefix fields.

The requested count is a nonzero upper bound on the available initial
fields. A shorter complete record exposes all of its fields; its complete
count remains available to comparison. Thus a search key longer than the
stored record still returns corruption through the canonical comparator,
not a new caller-misuse result. Zero requested fields are caller misuse.
Bytes beyond the required prefix may be supplied up to the declared complete
size, but the view borrows only the required prefix.

The complete header is still validated, including serial types after the
requested prefix, schema-format restrictions, structural/field limits,
checked payload-size arithmetic, and exact agreement with the declared
complete record size. Prefix parsing also proves every available body byte
needed by its cursor. Missing required prefix bytes and malformed encoded
metadata remain corruption.

Extract and reuse the current header-validation mechanism for both
`RecordView` and `RecordPrefixView`. Do not add a second varint decoder,
serial-type interpreter, field decoder, or persistent format. The format
module receives only byte spans and sizes and performs no Pager or VFS I/O.

Add a `CompareIndexRecord` overload for `RecordPrefixView`. Both view forms
use the same comparison implementation, `RecordCursor`, field comparator,
collation, numeric/storage-class handling, direction, NULL placement, and
`EqualPrefixResult`. Preserve existing metadata/search-width validation.
Check a complete-field shortage as corruption before checking an
available-prefix shortage as misuse; never iterate unavailable fields.
Comparison metadata may still omit unprobed stored suffix fields.

### 2. Retain only header and compared fields during index comparison

Keep the existing public `IndexBtreeCursor::Seek()` and `CompareCurrent()`
interfaces and their lower/upper, forward/reverse, equivalent-prefix, and
error behavior.

For a fully local record, continue comparing the borrowed complete record.
For an overflow record:

1. perform the existing complete-payload feasibility checks;
2. determine/read its complete header;
3. compute the exact bytes needed for the search-key field prefix;
4. read only those bytes into canonical `ByteBuffer` scratch when they are
   not already local; and
5. compare through `RecordPrefixView` and the shared comparator.

Scratch belongs to one synchronous seek or current-comparison operation,
not to `IndexBtreeCursor::Impl`. A seek may reuse that one buffer between
candidate comparisons, but releases it on every return, including error.
No encoded payload, prefix buffer, borrowed record view, or field view is
retained by the cursor after comparison.

The required contiguous comparison working set is bounded by the existing
record/header limits and the actual compared values. Registered collations
still receive complete compared TEXT values. This does not promise a
page-sized bound for an arbitrarily large indexed value, nor copy an
uncompared covering suffix merely to make it contiguous.

`ByteBuffer` remains the sole owned raw-byte representation. There is no
new vector scratch exception, payload cache, private comparator, reader
callback framework, or alternative seek algorithm.

### 3. Preserve complete overflow error visibility

Factor the existing cell-payload walker to support a comparison-prefix read
that copies a bounded initial range while traversing the complete payload's
required overflow chain.

The prefix mode preserves the current full-read checks and I/O reach:
snapshot validity, geometry, declared payload feasibility, page references,
premature links, malformed required overflow pages, and read errors. Merely
having a local key does not skip validation of an overflow covering suffix.
Do not silently fall back to the old complete-copy path after an error.

Staged format validation cannot return an error before the complete required
chain has been traversed successfully. A chain/read failure takes precedence
over a staged malformed-header, serial-type, body-size, or stored-width
error, just as the existing complete read precedes record parsing. Initial
snapshot/feasibility checks and scratch allocation failures retain their
existing early-error roles. Header acquisition may require a bounded first
read before the final prefix read; both use the same walker.

The last required overflow page is read, but its unused next-link value is
neither decoded nor resolved. This preserves ADR-0024 and SQLite behavior,
including a nonzero, invalid, or cyclic unused final link.

Ordinary `ReadPayload()` range semantics and `CopyPayload()` full-record
semantics stay unchanged. Both use the same payload walker; the new mode
does not acquire another traversal algorithm or a separately owned page
chain. It uses the existing bounded read pins and releases them under the
same snapshot/lifetime rules.

This is not a general corruption-detection expansion. Preserve checks
performed by existing complete comparisons rather than opportunistically
changing unrelated chain, writer, or Pager contracts.

### 4. Preserve ownership and resource boundaries

All codec views borrow caller-owned bytes. Scratch/view lifetimes end before
the comparison returns or before the scratch buffer is replaced for another
candidate. Error unwinding cannot leave a cached view of released storage.

Operation-local format/comparison scratch is a bounded B-tree working set,
not retained statement storage. It cannot survive a VM instruction. A
temporary relation's separately copied current record continues to use its
existing retained-memory account and bounded file-mode drain reservation.
No temporary-account callback or upper-layer owner is passed down into the
record codec or B-tree layer.

Do not change main-database pages, persistent encodings, native fixtures,
schema metadata, planner choices, bytecode, source cursor ownership, SQL
ordering, or result values. Existing ordinary, aggregate, MIN/MAX, joined,
and private-query consumers reach the corrected canonical cursor directly.

### 5. Keep automatic-source design gated

The subsequent automatic-index ADR must resolve the remaining reviewed
contracts before implementation:

- include one-time source scan and the actual construction backend in cost
  admission; do not cost adverse ordered-memory insertion as logarithmic;
- use the existing shared arena from the beginning of file-mode lookup
  insertion rather than paying ordered-array movement before migration;
- describe complete-record comparison metadata, separate from the probed
  equality-prefix width; and
- clear existing source cursor caches before moving or clearing a borrowed
  relation.

Those changes do not belong in this storage/format prerequisite. No
automatic-source behavior is claimed complete by this ADR or its PR.

## Verification

Start from the green complete fast baseline. Before production changes, add
and observe a focused failure showing that a one-field comparison currently
copies a large overflow covering suffix. Collect copy work only around
seek/current comparison, outside fixture setup and result copying. Keep the
existing VM instruction controls and later automatic-source scaling
measurement unchanged.

Format-owned tests use independent encoded vectors for:

- required header/prefix sizes and complete/available field counts;
- zero-payload integer constants, integer widths, real/NaN normalization,
  TEXT/BLOB, NULLs, BINARY/NOCASE, direction, and NULL placement;
- comparison equivalence between complete and prefix views;
- malformed serial types outside the requested prefix, truncated varints,
  header/body disagreement, supplied bytes beyond the declared total, schema
  restrictions, excessive field counts, invalid prefix widths, and
  insufficient available bytes.

Storage-owned tests cover:

- local keys with large overflow covering suffixes;
- headers or compared values that themselves cross overflow boundaries;
- all seek modes, equivalent prefixes, interior/leaf traversal, current
  comparison, clear/reseek, and retained-pin cleanup;
- malformed later overflow links and injected later-page read errors even
  when the compared key is local;
- staged header/serial-type/body-size errors combined with a later required
  chain or injected read failure, proving the traversal failure wins;
- comparison-path acceptance of nonzero, invalid, and cyclic unused final
  links, through both seek and current comparison;
- bounded copied bytes rather than total covering payload size; and
- deterministic OOM and existing cursor failure/snapshot behavior.

Allocation-lifetime tests must also hold the cursor alive while proving that
no comparison-owned allocation remains after successful or failing
`Seek()` and `CompareCurrent()`. A cursor-owned bounded prefix cache would
pass the copied-byte red but must fail this separate invariant. Include
failure after scratch allocation and repeated comparisons, not just cursor
destruction.

Reuse the existing B-tree fixture/build helpers, record golden vectors,
allocation-failure harnesses, instrumentation, and pinned compatibility
fixtures. Do not create or mutate a frozen fixture to obtain a passing test.

Run the focused and complete fast tiers, then required Debug, Release,
ASan/UBSan, genuine GCC Debug/Release, pinned formatting, unfiltered
production clang-tidy, engineering discipline, dependency-graph validation,
native comparisons, sanitized fuzz, and all frozen performance contracts.
No performance claim substitutes for correctness or complete malformed/I/O
coverage.

Obtain focused design, implementation, and exact-tree closure review.
Publish and merge only the reviewed tree after all exact-head hosted gates.
Then resume the measured automatic-index source design. Node 43 and all
later ADR-0053 slices remain incomplete.

## Related decisions

This decision narrowly amends ADR-0018's complete-view-only comparison
surface and ADR-0024's complete-copy, cursor-owned overflow scratch policy.
Their complete-record validation, payload APIs, and unused-final-link
contracts remain in force. It provides the lower-layer prerequisite required
by ADR-0053's automatic-index slice and ADR-0058's retained-byte ownership
contract; neither SQL source integration nor temporary budgets change here.

## Rejected alternatives

- A complete retained payload buffer would leave temporary cursor memory
  outside its canonical account.
- A prefix-only overflow walk would hide existing corruption or I/O errors.
- Treating incomplete bytes as `RecordView` would invalidate its decoding
  proof and callers' lifetime expectations.
- A second field comparator, decoder, B-tree walker, or byte container would
  duplicate accepted mechanisms.
- A temporary-storage budget interface in the format/storage layers would
  reverse dependencies and introduce upper-layer ownership into comparison.
- Combining this prerequisite with the new planner, relation lifecycle,
  bytecode, source borrowing, and LEFT integration would make the PR harder
  to review and obscure its independent resource contract.
