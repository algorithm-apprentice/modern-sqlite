# ADR-0145: Native Where-Path Ordering and Block Producers

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entries 9 and 12

## Remaining producer boundary

PR #247 closes complete ordinary single-table input ordering, not joined or
prefix-order publication. Existing native fixtures confirm:

- a joined `ORDER BY a.id LIMIT 2` can return two rows before a later error,
  while Modern still constructs the entire ordered input;
- an indexed `ORDER BY category, score+0 LIMIT 2` can return early blocks,
  while Modern reports a much later predicate error before any row;
- an error in the first row of the next block can occur before output from
  the previous block: block sorting has genuine lookahead; and
- a top-N rejection in an ordered innermost loop can skip its remaining
  rows even when the complete joined stream requires sorting.

The frozen row/error controls are retained in session evidence. Do not turn
these differences into SQL-text exceptions or stop an unordered producer
after an arbitrary number of input rows.

## Native and existing mechanisms

| Responsibility | Local pinned SQLite / canonical Modern owner |
|---|---|
| Full/prefix/unknown order, fixed terms, order-distinct sources, reverse mask | `where.c:5150-5488`; existing `WhereLoopBuilder` and source-owner visitors |
| Sort costs and bounded two-pass N-best solving | `where.c:5539-5599,5846-6270,7089-7130`; existing `SolveWherePath`, LogEst and loop candidates |
| Star adjustment once and interstage search protection | `where.c:5666-5798,6274-6350` |
| Order-mask admission and RIGHT replay override | `where.c:6876-6881,7437-7443` |
| Ordered-inner-loop rejection target | `where.c:80-113,6221-6237` |
| Prefix record construction, lookahead, drain and reset | `select.c:718-887,1227-1340,1726-1948` |
| Reusable record register and nonoverlapping inputs | `vdbe.c:3577-3920`; existing `SqlValue`, `SqlValueAccess`, record codec |
| Prefix owner transfer | `select.c:820-841`; `expr.c:4601-4602`; `vdbe.c:1661-1696`; `vdbemem.c:1240-1249` |
| Sorter/B-tree block reset without reopening | `vdbe.c:7181-7195`; existing temporary-storage reset primitives |
| Projection, traversal, admission, publication and cleanup | Existing `PlanLowerer` producer, `RecordSorter`, `BoundedTopN`, typed VM |

This extends ADR-0120's ordering admission only when the corresponding
producer is delivered. It does not replace its candidate search, source
ownership, budgets, term consumption or one selected source sequence.
The new path/publication admission concerns ordinary nonaggregate ordering.
Existing DISTINCT, grouping and MIN/MAX contracts remain authoritative;
do not apply their less-strict grouping proof as an ORDER BY proof or add
ordered DISTINCT as an incidental optimization.

## Decision

### 1. Stage a reusable encoded record through existing value ownership

Add typed encode-record, insert-encoded-sorter and insert-encoded-top-N
instructions to the same VM. The encode instruction consumes an initialized
positive register range and defines a distinct BLOB register; output/input
overlap is rejected as in the native opcode contract. It does not apply
table affinity, invent a format or perform I/O.

Reuse `EncodedRecordSize` and `EncodeRecordInto`. Generalize the existing
private retained-byte preparation/commit primitives rather than introducing
another byte container. Repeated construction reuses the record register's
`ByteBuffer` capacity. Commit the BLOB only after successful encoding;
existing value/record size limits and typed allocation errors remain.

The encoded insertion instructions synchronously borrow the BLOB's
`ByteView`. Add explicit borrowed encoded-record entry points to the
existing sorter/top-N implementations. Existing owned-buffer entry points
delegate to that same implementation while retaining their argument owner
until return. There is no second sorter, encoder, pending-record registry
or ownership-transfer fallback.

Top-N queue wrapping reuses the current stable borrowed-BLOB representation
only until canonical queue encoding completes; clear borrowed fields before
return or canonical failure cleanup. Generalize the existing borrow
primitive, not a new value representation. Sorter spill completes its use
of borrowed input before returning.

Keep the direct value-span sorter path from ADR-0116 unchanged for ordinary
sorting. Encoded staging is required here because the next block's complete
record, including allocation/size errors, precedes the previous block's
drain. Delaying encoding until after that drain would change observable
error order.

Add a typed register-range ownership move for the subsequent prefix handoff.
Validate positive, initialized, in-bounds, nonoverlapping source/destination
ranges through the same operand and transfer analysis. Reuse `SqlValue`'s
existing noexcept move assignment: release the old destination, transfer the
already-evaluated owner without allocation/coercion, and leave each source
as initialized SQL NULL. Both ranges are writes for coroutine ownership and
ordinary-call summaries. Do not substitute deep `CopyInstruction` or retain
an ephemeral prefix borrow across row evaluation or drain yields.

### 2. Extend the existing bounded solver with native ordering state

Use the same loop candidates and N-best workspace. Carry unknown versus
known prefix length and the reverse-loop mask in each path. Preserve native
compatibility classes, final-round coalescing, replacement/tie order,
1/5/12/18 widths and automatic-index setup rules.

Run the unsorted estimate first, apply the native interstage protection of
selected searches, then run ordered solving with the first result's signed
row estimate. Apply star adjustment once. Use native sort-cost arithmetic,
fixed-limit admission and prefix discount, not a new work score or a
LIMIT-plus-OFFSET estimate substituted for native `nSelectRow`.
Recognize native signed 32-bit integer syntax through the existing bound
expression owner shared with one-row demand. Only unary signs may wrap
the integer literal; booleans, collation and likelihood wrappers do not
become fixed limits merely because their evaluated values are integers.
Removing an oversized ordinary ORDER BY request also clears the native
ordinary-DISTINCT estimate request, without changing aggregate input requests.

Generalize the current source-order proof instead of retaining it beside
another algorithm. Reuse normalized constraint scanning, affinity/collation
checks, source masks and `IsOrderingConstant`. Captured-only expressions are
not source-free ordering constants. Equality-fixed terms are admitted only
when their key prerequisites are available. Order-distinct rowid or UNIQUE
NOT NULL prefixes govern which inner sources and dependent terms may follow.

Reverse bits refer to selected loop depth, not declared source IDs. Publish
direction through existing access nodes and teach the existing joined
traversal to consume it; retain the canonical seek/end-key phases.
RIGHT/FULL replay forces the published input-order prefix to zero at the
native boundary. Do not claim ordered output merely from the solver's
earlier proof.

Replace the single-source post-selection ordering search once the same
canonical path evaluator covers it. Do not retain parallel cost/proof
implementations. Source scopes and immutable query owners remain unchanged.

### 3. Consume prefix order through the canonical block producer

Publish a checked prefix count in the existing physical order nodes rather
than retaining both a count and an independently mutable satisfaction bit.
Complete order is derived from that count and the requested term count.
Update construction, validation, explanation and consumers directly.

Omit satisfied prefix fields from the stored sort key. Result columns that
were represented only by those keys become ordinary payload fields. One
canonical field-mapping helper supplies descriptor construction and output
draining; no second record layout or encoder is added.

The producer evaluates a qualifying next row in native order, encodes its
complete pending record, then compares its prefix with the previous prefix.
On a change, drain the old block before admitting the pending row. Apply
the native remaining-LIMIT check after that drain, not before evaluation of
the lookahead row. After block reset and that check, move the lookahead's
prefix into the retained previous-prefix range using the allocation-free
range move. The pending encoded record already owns any required prefix
payload, and is unaffected when the key source registers become NULL.
Keep global OFFSET/LIMIT across blocks.

Reuse the current sorter and bounded B-tree relation. Block reset clears
contents without reopening roots or losing the pending record register.
Top-N block reset updates the remaining bound while preserving its native
statement sequence; ordinary whole-query reset retains its existing
semantics. Preserve memory/file budgets, overflow handling and cleanup.

Use one canonical C++ drain emitter at the prefix-change and final-input
sites. Their distinct cursor-capability states must not be merged through
an unverified call or weaker lattice. Client rows and coroutine yields stay
inside their verified lexical region. Do not add a yield-capable subroutine
ABI or another runtime executor for this purpose.

### 4. Preserve ordered-inner-loop cutoff

Use the same order evaluator's native innermost-loop mode. Publish the
rejection continuation through existing joined-loop targets. A rejected
top-N row may finish the innermost loop only when that proof applies;
RIGHT replay retains the ordinary continuation. Predicates, projections
and errors before admission remain in their original phases.

## Ordered delivery

Use one related batch, normally four reviewed source commits:

1. reusable encoded-record staging, range ownership move and synchronous
   encoded insertion;
2. native path ordering and complete joined publication/traversal;
3. prefix layout, block draining/reset and corresponding partial sort cost;
4. native ordered-inner-loop rejection continuation.

Until source 3 exists, retain ADR-0120's rule that a prefix requiring an
unimplemented block producer is costed as mandatory full sorting. Do not
publish a runtime partial-order capability before its consumer qualifies.
This is a bounded delivery prefix, not a completed partial-sort alternative.

Current unsupported access kinds remain unsupported. Non-natural NULL
two-pass traversal and any unimplemented automatic-index reverse traversal
need their own completed source mapping/admission before being treated as
delivered order. No later SQL grammar or DAG node is introduced.
The actual compound coroutine integration from ADR-0143 remains separate
and blocked by its remaining producer prerequisites.

## Evidence

Each source requires a focused red/green or green-refactor baseline,
complete fast units, current ownership/malformed/OOM/fault tests and
GPT-5.6 Sol review before its normal commit and append-only ledger entry.
Cover retained-byte reuse, borrowed-input lifetime, record limits/alias
rejection, allocation-free text/BLOB range moves and source NULLs, growing
lookahead prefixes across yields, mixed-direction joins,
fixed/captured/nullable terms, both solver
passes and ties, RIGHT replay, genuine block spill, pending-record failures,
LIMIT/OFFSET lookahead, sequence/bound reset, scalar demand and recovery.

The qualified same-tree PR #247 capture is the before baseline. Preserve
frozen oracles, native identity, workload/guard settings, allocation ceilings
and original deadlines. Run the actual change-aware final batch plan,
matched fixed work/timings and hosted gates; publish the complete ledger
before integrated acceptance. Node 43 and the blocking audit remain open.
