# ADR-0071: SQLite-Faithful Buffered PMA I/O

- Status: Accepted
- Date: 2026-10-12

## Context and admission

ADR-0070's complete unoptimized aggregate baseline from source `96da210`
preserves all fifteen cases, three paired rounds, and the 40x wall/CPU guard.
Only `sorted-group-file` fails: aggregate wall ratio 45.737347 and CPU ratio
45.609993, with every paired round between approximately 45.3x and 46.1x.
The equivalent memory-grouping case is approximately 4.64x.

The diagnostic file case records 262,159 VFS calls for one 65,536-row query.
Current PMA output writes its length header and record separately; current
drain reads a header and payload separately for every record. Small records
therefore cross the VFS boundary repeatedly even though the extent stream is
sequential.

The pinned local SQLite source uses page-sized `PmaWriter` and `PmaReader`
buffers in `src/vdbesort.c:493-609`, `637-673`, and `1626-1708`. Normal records
and varints are served from the current page buffer; boundary-crossing data
uses bounded owned assembly. This is the admitted mechanism, not an unmeasured
new storage algorithm.

## Decision

### One coherent native mechanism

Buffer both PMA output and PMA input using the source database page size.
Use the existing owned `ByteBuffer`, borrowed byte views, checked varint and
record decoders, shared sorter extents, merge heap, and statement memory
account. Do not add a second file, merge algorithm, grouping container, mmap
path, configurable tuning layer, or persistent format.

The buffers remain private to temporary storage. Lowering, VM, session, and
SQL contracts are unchanged. No record validation, extent boundary check,
stable comparison, or explicit error is removed.

### Output ordering and ownership

One PMA writer borrows its exact output extent and owns one page-sized buffer.
Append encoded length then payload into the buffer, flushing full chunks in
source order. Flush the final partial chunk before publishing the completed
PMA run or merge output. Never flush from a destructor.

No write may touch bytes before the extent start or after its declared end.
An extent may begin in the middle of a physical page; preserve surrounding
owners' data by writing only the writer's buffered logical range.

Allocate and charge the fixed buffer before it becomes live. During an
already-selected spill, use the documented bounded file-mode working-set
accounting where ordinary admission cannot reserve the page. Release that
charge and owned memory on success, OOM, checked write failure, and destruction.
An append or final-flush error poisons the existing statement and does not
publish the incomplete run.

Every allocation and capacity-reconciliation delta in a selected spill or
intermediate merge reserves its bounded working set directly. This includes
the output page, cursor/base/heap arrays, reader pages, and current records,
from before input pointers are borrowed until run bookkeeping is restored.
Ordinary pressure selection must not recursively rewrite those same input
runs. Final drain retains ordinary admission followed by the existing
file-mode working-set fallback, only after the sorter is positioned. All
these retained capacities remain charged to the canonical sorter account;
stack writer state remains excluded under ADR-0058.

Diagnostic successful PMA spill-write events describe actual successful
buffer flushes after buffering, not records merely appended in memory.

### Input bounds and lifetime

Each PMA reader owns one fixed page-sized read buffer, charged alongside its
existing merge state. Refill only from the reader's owned extent and cap the
request at extent EOF. Cache logical position and the valid buffered range;
read-ahead never includes another live extent.

Read header bytes across page boundaries through the same buffered cursor.
Retain the existing complete varint, representable length, extent-end, and
record-format checks. A short refill is corruption; VFS errors propagate
unchanged and poison statement storage.

Keep each current record in its existing independently owned `ByteBuffer`.
Do not introduce borrowed record views into a refillable buffer in this
optimization. Copy buffered payload slices into that owned record while
preserving its existing charged lifetime and decode contract. Validate the
decoded field count against the sorter descriptor before publishing a record
to the merge comparator.

Readers release both record and page charges exactly once on EOF, replacement,
merge failure, reset, close, and statement destruction. The finite merge
fan-in of 16 bounds page working-set memory.

### Tests before implementation

Add a focused failure proving sequential PMA write/read calls are bounded by
bytes divided by page size rather than rows, while row order and stable ties
remain unchanged.

The initial 512-row test observes 1,024 writes and 1,024 reads before buffering,
with sorted output already correct. Both call-count assertions fail as
expected; the complete red output is retained with the pre-optimization
session evidence.

Cover small records, varints and payloads crossing buffer boundaries, a record
larger than the buffer, mid-page shared extents, short headers/payloads/refills,
malformed lengths, exact extent EOF, OOM at each new allocation, and failure
at full/final output flush and input refill. Use existing deterministic VFS
and temporary-storage test helpers.

Buffering deliberately changes which VFS call observes an injected read/write
failure. Tests specify the refill/flush boundary rather than demanding a call
on every `Next()`. No failure is silently ignored or converted to successful
EOF, and existing statement poisoning/cleanup remains mandatory.

### Integrated performance gate

Keep ADR-0070 fixture, SQL, fixed repetition work, result digests, diagnostic
operation counts, thresholds, and 40x guard unchanged. Preserve the complete
failed pre-optimization evidence. Record the post-change baseline from a
reviewed clean source commit, including fixed-work VFS-call reduction.

Run all aggregate correctness, malformed, fault, OOM, ownership, model, fuzz,
compiler, sanitizer, and quality checks plus existing non-target performance
contracts. Merge no package whose final evidence still fails the admitted
guard.

The complete post-change recording from clean reviewed source `52a9e84` is
`benchmarks/aggregate-baseline-v1/`. The sorted file case passes all three
paired rounds, with aggregate wall 6.908375x and CPU 6.887353x. Its diagnostic
records 385 VFS calls and 185 actual successful PMA flushes for the unchanged
65,536-row query. All fifteen cases pass the original 40x aggregate and
every-round guards. No additional tuning is admitted by these results.

## Rejected alternatives

- Raising the file-mode budget or performance guard hides the measured gap.
- Removing corruption/extent validation trades correctness for timing.
- Buffering only output leaves per-record read calls in the same admitted
  sequential-I/O cluster.
- mmap adds a separate lifetime/platform mechanism when the proven page
  buffers suffice.
- Borrowing decoded records directly from the read buffer expands invalidation
  risk without being required for this bottleneck.
- A byte-vector scratch implementation duplicates the canonical `ByteBuffer`.

## Traceability

Node: `implement-advanced-sql`, aggregate package 15 closure.

Contracts: ADR-0008, ADR-0054, ADR-0058, ADR-0070.
