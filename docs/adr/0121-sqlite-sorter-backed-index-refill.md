# ADR-0121: SQLite Sorter-Backed Index Refill

- Status: Accepted
- Date: 2026-10-20
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 10 (`write-create-index`)

## Decision

Translate the pinned `sqlite3RefillIndex()` pipeline for the existing
column-only rowid-table CREATE INDEX surface. Use the canonical sorter and
writable B-tree; do not add an index-only sorting implementation or retain
point insertion as a bulk-build fallback.

| Responsibility | Pinned source |
|---|---|
| Refill owners, scan/sort/drain, uniqueness, close order | `build.c:3851-3950` |
| Stable logical-key sorting with the rowid suffix omitted from comparison | `vdbesort.c:1065-1138` |
| Reusable previous encoded record and current-key NULL handling | `vdbesort.c:2880-2955`; `vdbe.c:6170-6227` |
| SeekEnd and adjacent insertion without another key search | `vdbe.c:6390-6440,6728-6775`; `btree.c:9400-9750` |
| Bulk sibling selection, right packing, and nearby-page allocation | `btree.c:8277-8357,8635-8715,9259-9261,6710-6784` |

The native sorter owns records and merge state. One mutable index cursor
retains its page path. The previous-row register owns one reusable encoded
record, not decoded SQL fields. A duplicate non-NULL logical prefix aborts
before the next record is copied or inserted. These owners remain bounded
and are released before statement rollback.

## Canonical mechanisms and boundaries

- Reuse `RecordSorter`, direct value insertion, stable memory/PMA ordering,
  `RecordView`, `CompareRecordPrefixes`, `ByteBuffer`, statement temporary
  storage, and existing collations/record options.
- Reuse `WritableCursor`, `MutationPageOwner`, the existing mutable-page
  insertion/balance algorithms, root/statement epochs, and rollback marking.
  Extract the existing retained table-mutation cursor owner into one private
  shared mechanism rather than duplicating its ownership/validation protocol.
- Preserve ADR-0108 raw stored INTEGER/REAL serial forms. Table fields use the
  existing raw CREATE INDEX read descriptors and synthesized-default affinity;
  no new affinity pass occurs before sorter encoding or persistent insertion.
- Do not change DML index maintenance, ANALYZE, SQL grammar, REINDEX,
  expression/partial indexes, WITHOUT ROWID indexes, conflict actions, or
  transaction/session policy.

### Retained encoded append cursor

Add a move-only `IndexBtreeAppendCursor`, opened from `IndexBtreeWriter` for an
empty index root. It owns the canonical retained mutation state and accepts
a borrowed validated `RecordView` through `AppendSorted()`.

The caller's explicit trusted precondition is strictly increasing complete
physical keys. CREATE INDEX establishes it with stable logical-key sorting
of a signed-rowid-ascending table scan; the final rowid suffix orders ties.
This is the native SeekEnd/USESEEKRESULT precondition, not permission to
suppress record validation, root/epoch validation, or failure cleanup.
The append cursor validates complete field count and encoded input boundaries;
it does not add a second key-search or last-key container.

Metadata verification cannot prove the runtime admission order of a directly
constructed public bytecode program. Before append, the VM therefore checks
complete physical monotonicity using the same previous encoded record needed
by native uniqueness checking: compare logical prefixes once, require an
increasing integer rowid suffix on an equivalent prefix, and reject a
decreasing prefix or non-increasing complete key with a typed misuse error
before insertion. A duplicate non-NULL UNIQUE
prefix still reports the existing constraint error before copying/insertion.
This narrow guard is required by ADR-0032's public bytecode trust boundary;
SQLite's private VDBE producer invariant is insufficient here. It adds no
B-tree seek or second key container. Bare storage callers retain the explicit
`AppendSorted()` precondition.

SeekEnd keeps an already-valid rightmost leaf path. After balancing invalidates
the position, descend to the rightmost leaf through the same cursor. Append
at its cell count through the canonical formatted-cell insertion. Retain the
inserted position only for an unbalanced append; existing ordinary mutation
behavior is unchanged.

Carry the native bulk hint through `BalanceNonroot()`: select two adjacent
siblings where ordinary balancing selects three, retain left-packed bulk
pages while still preventing an empty right sibling, and use nearby page 1
when allocating surplus bulk siblings. Extend the existing freelist allocator
with that narrow nearest-page request; do not add another allocation path.
All staged-cell, overflow, pin, incarnation, and rollback rules remain intact.

### Sorter-backed bytecode pipeline

Extend the existing ordering descriptor with an optional
`index_refill_cursor`. It is valid only for a CREATE INDEX sorter targeting
one pending index root. Its record has all physical fields, while its
comparison columns are exactly the logical key terms. The bytecode verifier
checks field counts, symbols, order, uniqueness metadata, target ownership,
and rejects this association on top-N or relation descriptors.

The fixed association also selects physical index ordering at runtime:
schema formats before 4 ignore DESC for persistent indexes, unlike query
ORDER BY. Use the same runtime ordering-descriptor adapter with that explicit
distinction; do not introduce a second comparator.

Lower CREATE INDEX as:

1. Create the root and schema row through the existing instructions.
2. Open the shared sorter and table cursor.
3. Read each raw physical key plus rowid and insert it directly into the
   sorter's canonical owner.
4. Rewind the sorter. For an associated refill sorter, this first opens the
   retained append cursor, matching native OpenWrite-bulk before SorterSort.
5. Loop one new `InsertSorterIndexInstruction` and the existing sorter Next.
6. Close table, index, and sorter, then advance the schema cookie.

The new row instruction performs the cohesive native
SorterCompare/SorterData/SeekEnd/IdxInsert stage. It borrows the current sorter
record, compares its logical prefix with the previous record once for order
validation, and applies uniqueness only when the current key contains no
NULL. It then copies into one reusable `ByteBuffer` before appending the
encoded record. Retain a separate logical encoded-record length alongside
that high-water buffer; comparison, parsing, and append use only
`view().first(logical_size)`, never stale trailing capacity. The buffer grows
only when necessary and remains subject to VM value limits. No per-row owned
decode, encoding, uniqueness probe, or temporary record vector is introduced.

Track the append phase in the existing packed write-cursor state. Ordinary
index writes cannot mix with an active refill. The row instruction requires
a positioned associated sorter and its matching append writer. Close/reset
destroys refill state before its index writer; failure abandons the normal
VM execution context. Every CREATE INDEX retains native multi-write/ABORT
effects, including nonunique builds.

`WriteSession` already attaches the canonical temporary-storage factory.
Direct VM callers of a sorter-backed CREATE INDEX must supply that existing
capability, just like other sorter consumers; absence remains an explicit
misuse error rather than selecting a silent backend. The IF NOT EXISTS
no-op remains a zero-resource halt.

## Persistent ordering and evidence

Root/schema creation remains before the source scan. Main-index population
begins only after complete sorter admission and ordering. Duplicate detection
precedes copying/insertion of the duplicate row. Cookie/catalog publication
follows complete population and cursor cleanup. Root, schema, population,
temporary-file, allocation, and cookie errors use the existing anonymous
savepoint/transaction rollback path; no partial index or schema is published.

Deliver two ordered implementation slices: retained encoded append/bulk
balancing, then canonical sorter-backed refill. Each has focused red/green,
complete fast units, GPT-5.6 Sol review, and an append-only ledger record
before its successor. Publish the draft after the accepted design/first
accepted commit; run the final actual-change qualification once on the final
production head, with native contracts early.

Reuse existing writer, lowerer, session, OOM, crash, serial-form, native
interoperability, and temporary-storage tests. Add only focused coverage for
append boundaries/splits, stale ownership, bounded allocations/search work,
mixed collations/directions/NULLs, older schema formats, duplicate rollback,
memory/file spill, and borrowed-record lifetime. Direct-bytecode tests include
equal logical prefixes admitted with descending rowids, duplicate complete
keys, and a large record followed by a shorter record in the reused buffer.

The frozen before evidence is ADR-0120's unchanged fifteen-case index run at
`cc442a2`, whose 406 production/test/tool/configuration entries are identical
to merged base `efe0522`. CREATE INDEX records 85,565 diagnostic allocations
and a 6.80x wall / 7.39x CPU ratio. Preserve its inputs, thresholds, and
deadlines; capture fresh matched after evidence without replacing history.

## Retained append slice evidence

The focused append-cursor test first fails because the required API is absent
at red tree `51554e03023792b748efa2794125fbbd4437270f`. The implemented cursor
appends 1,024 encoded records without invoking their text comparator and
matches every interior/leaf cell count in an independently generated pinned
SQLite 512-byte-page bulk-index golden. No binary fixture is added.

Focused coverage also exercises malformed field counts, moves, rejection of
nonempty roots, cursor ownership beyond the writer facade, required pin
release before commit, 1,400-byte overflow records with a four-page cache,
and nearby-page selection without changing ordinary freelist order.
Exhaustive allocation failure includes retained-cursor creation and twelve
overflow appends, with exact original-image rollback after every failure.
All 76 affected storage cases pass in Debug, GNU Debug, and ASan/UBSan.
Production quality, format, discipline, and graph checks pass. The initial
fast report selected 1,467 ordinary and 92 isolated cases, but its complete
source-qualification claim is superseded by the stale-link correction below.
This accepted slice was
committed as `d7ced2e` before sorter-backed VM/lowering integration began.

## Sorter-backed refill slice evidence

The focused producer test first fails at tree
`815e40fb9627668a9680e16a537d984ac5e927fc`: CREATE INDEX has no shared sorter.
The translated pipeline passes all 108 affected verifier, write-VM, and
CREATE INDEX cases. Its initial fast report selected 1,477 ordinary and 92
isolated cases; the stale-link correction below supersedes the complete
source-qualification claim. Existing raw REAL/default and UNIQUE rollback tests remain.
Direct bytecode controls cover strict rowid ties, nullable UNIQUE keys,
constraint precedence, encoded value limits, large-to-short buffer reuse,
actual PMA spill/read/write failures, and legacy physical DESC suppression
without changing query ordering. The existing VM OOM runner exhausts creation
and every Step allocation through sorter admission, buffer growth, and
overflow append, restoring the exact original image after each failure.

The fixed sorter association also participates in the existing private
resource-ownership check, just as a borrowed relation cursor does. Its focused
control first reports the later cursor-open error rather than the required
cross-scope ownership error; propagating the association through the same
ownership check makes the control pass. No second ownership table or
comparison-column owner is added: refill uses `RecordSorter::CompareCurrent()`.

## Bounded emission-capacity correction

- Amendment status: Accepted

Fully relinked isolated Debug at `52f7fa9` passes 1,610 of 1,611 cases but
exposes CREATE INDEX lowering at 43 allocations against its original exact
36-allocation contract. Earlier selected-target builds had left some static
standalone runners stale: `run_fast_tests.py` executes binaries but does not
build them. Those fast claims are superseded, not backdated. Build the entire
development preset before every complete fast run and requalify the dependent
suffix after this correction.

Keep the native sorter pipeline, required metadata, and original allocation
contract. Its complete emission has one instruction per key term plus 25
fixed instructions and four branch labels. Reserve those known capacities
through narrow `ProgramBuilder::ReserveInstructionCapacity()` and
`ReserveLabelCapacity()` operations on the existing pending-instruction and
label vectors. Reservations check builder validity and existing identity/count
limits, publish no instruction or label, and do not change owned-byte
accounting. The no-op requests no capacity. No alternate builder, publication
owner, compacting factory, or index-only compiler path is introduced.

This removes seven avoidable small-vector growth allocations rather than
raising the contract: six pending-instruction allocations become one and
three label allocations become one. It follows the pinned VDBE's block
allocation intent in `vdbeaux.c:164-211,1147-1182` and delayed label-block
allocation in `vdbeaux.c:615-657`, through the canonical Modern owners.
The unrelated public input-copy/canonicalization contract remains unchanged.
The existing allocation runner supplies the focused red; capacity/count,
moved-builder, OOM, full-build fast, and final exact-head gates close the
correction. Retain the unamended earlier commits and failed reports for
traceability; only the final fully qualified tree may be squash-merged.

The capacity tests first fail because both reservation operations are absent.
The corrected producer passes the unchanged exact 36-allocation contract.
A complete default development build finishes all 81 pending compile/link
steps before the fresh fast run: all 1,479 ordinary and 92 isolated cases pass.
This report, unlike the superseded reports, includes freshly linked standalone
runners.

## Final local closure

The corrected production head is `0794a1d` (tree
`7878da2bb7862ce0522805bba242a30db1f6e97e`). Exact-head qualification passes
27 reconciled shared checks, 58 native contracts, 1,613 isolated cases in each
of Debug, Release, ASan/UBSan, GNU Debug and GNU Release, and 1,630 instrumented
cases. All four SQL/database read/write fuzz campaigns complete their unchanged
20,000 runs with seed 102. No selected case is skipped or disabled. The
original ten-second sanitizer correlated model completes in 1.31 seconds.
Final documentation may reuse this evidence only with an exact comparison of
all 1,954 non-Markdown tree entries.

The fifteen-case matched capture preserves the frozen workload, fixture,
SQLite, host and build identities. CREATE INDEX falls from 6.80x to 3.05x
SQLite wall time and 7.39x to 3.31x CPU, with allocations 85,565 to 46,437 and
cache hits 105,193 to 9,041. All non-target diagnostic counters remain identical;
their timing movements are not attributed to this repair. ANALYZE remains
25,074 allocations and 2.87x wall time, and covering execution remains a
separate ledger-12 gap. No whole-engine performance parity is claimed.

The original driver-summary parsing interruption and failed `52f7fa9`
allocation qualification remain historical evidence, not successful substitutes.
Only the freshly rebuilt corrected suffix supplies final local qualification.
Final integrated review and all nine exact-head hosted checks remain merge
requirements. Node 43 stays active; ledgers 11-15 still block SQL feature
expansion.
