# ADR-0105: SQLite-Aligned Rowid Range Access

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

ADR-0104 corrects conditional predicate branching. Its frozen 20-case audit
now has 19 exact matches. The sole remaining case is:

```sql
SELECT id
FROM audit_l
WHERE (NULL OR abs(id-id-9223372036854775807-1)) AND id<0
ORDER BY id;
```

Pinned SQLite publishes `SEARCH audit_l USING INTEGER PRIMARY KEY (rowid<?)`,
finds no candidate, and never reaches the earlier residual. Modern publishes
`SCAN "audit_l"`, evaluates that residual on the first row, and reports the
integer-overflow error. Reordering residuals would hide the difference without
implementing the reference access mechanism.

Begin after PR #214, main commit
`dee4020e73f61bd1b1b2349ed4e97278d55c1288`, accepted tree
`a684376db7308f865a50b59de0f6518e8b6a5d89`. All nine final-head hosted checks
passed. The original unchanged sanitizer model passed in 6.38 seconds under
its ten-second deadline. The macOS Debug tool-duration correction retained
every 30-second owner deadline.

Pinned SQLite's mechanism is:

- `src/where.c` admits lower and upper INTEGER PRIMARY KEY constraints;
- `src/wherecode.c:1768-1875` maps the logical start to `SeekGT`, `SeekGE`,
  `SeekLT`, or `SeekLE`, positions with Rewind/Last when no start exists,
  evaluates the logical end only after positioning succeeds, and checks the
  current rowid before each row body;
- `src/vdbe.c:4967-5055` applies numeric affinity to TEXT, handles INTEGER,
  REAL, NULL, and nonnumeric storage classes, adjusts inclusive/exclusive seek
  modes for fractional REAL values, and performs the B-tree table seek; and
- reverse traversal swaps logical start/end roles and uses Previous.

Current reusable Modern mechanisms are:

- `CollectConjuncts`, source-independent key analysis, stable conjunct
  positions, access candidates, costs, residual removal, and rowid equality;
- `PhysicalIndexBound` lower/upper metadata and the established index-range
  analyzer;
- `PhysicalTableScanNode` direction and one canonical table row loop;
- `TableBtreeCursor::Seek` with equal, greater-or-equal, greater,
  less-or-equal, and less modes;
- `SeekRowIdInstruction`, whose current public modes are equality and strict
  greater for exact lookup and mutation-cursor resume;
- existing numeric affinity, exact integer/REAL comparison, rowid reads,
  typed comparisons, composite conditional jumps, Next/Previous, and
  verifier cursor-state tracking; and
- ordinary, streaming, grouped/global aggregate, private scalar/EXISTS,
  DISTINCT/compound, ordering, and LIMIT consumers that currently reuse the
  same lower-level cursor and instruction mechanisms through distinct access
  analyses and loop emitters.

ADR-0036 explicitly deferred rowid ranges. Ordinary secondary-index ranges,
nullable range correction, joined index ranges, and mutation scans do not
authorize a second rowid executor.

## Decision

### 1. Publish one explicit physical rowid-range access

Rename `PhysicalIndexBound` to the storage-neutral `PhysicalRangeBound` and
reuse it for both index and rowid bounds. It continues to carry:

- the source expression key;
- the original predicate;
- conjunct position; and
- inclusive/exclusive mode.

Add `PhysicalRowIdRangeNode` with the same source/table/root identity as rowid
lookup, optional lower and upper `PhysicalRangeBound` values, and one
`PhysicalScanDirection`. Add a distinct `PhysicalAccessKind::kRowIdRange`;
do not overload equality lookup or represent a constrained scan as an
unconstrained table scan.

The first rowid-range surface is limited to nonjoined SELECT cores whose
source is an ordinary rowid table or `sqlite_schema`. Joined parameterized
rowid ranges, mutation access, WITHOUT ROWID tables, IN/OR unions, and
expression-derived monotonic bounds remain separate work.

Introduce one internal `NonjoinedCoreAccessAnalysis` for every nonjoined
SELECT core. It owns:

- resolved source metadata;
- guards and stable predicate terms;
- table, rowid-equality, rowid-range, and catalog-order index candidates;
- the selected candidate index;
- selected-term mask and residual predicates; and
- selected physical direction.

Use that analysis for ordinary reads, nonjoined aggregate cores, DISTINCT,
streaming and compound arms, and private scalar/EXISTS cores. Joined access
and mutation planning retain their existing independent contracts.

Publish one `PhysicalCoreAccessPlan` per analyzed nonjoined core in the owning
`PhysicalQueryPlan`. It exposes core index, candidate list, and selected
candidate before lowering. Existing simple-query `candidates()` and
`selected_candidate()` remain compatibility views of the root core's entry.
Chosen leaf/filter nodes remain the executable physical representation; no
access choice is made inside lowering.

### 2. Reuse canonical predicate analysis

Recognize `>`, `>=`, `<`, and `<=` after the existing whole-predicate
alias/COLLATE/likelihood transparency and the existing rowid-operand
alias/COLLATE rules. Unary plus remains an access barrier.

Exactly one comparison side must be the source rowid; the opposite expression
must be source-independent. Reverse comparison orientation maps to the same
logical lower or upper bound. Preserve the original expression ID and
predicate rather than synthesizing arithmetic or integer constants.

Scan conjuncts in source order. Retain the first lower and first upper match,
as the existing index-range analyzer does. Mark only selected terms and leave
every other term in residual order. Equality lookup remains independently
eligible and wins through its lower cost; do not merge equality and range
nodes.

For table estimate `N` and estimated row width `W`, publish the exact cost:

- if `N == 0`, estimated output and work are zero;
- otherwise estimated output `O` is `max(N / 4, 1)` for one bound and
  `max(N / 64, 1)` for two bounds;
- seek work is `bit_width(max(N, 1))`;
- row work is the saturated product `O * W`; and
- total work is the saturated sum of seek work and row work.

The published candidate has:

- kind `kRowIdRange`;
- `estimated_input_rows = N`;
- `estimated_output_rows = O`;
- `index = nullopt`;
- `covering = false`;
- equality-term count zero; and
- range-bound count one or two.

Stable candidate order is table scan, optional rowid equality, optional rowid
range, then catalog-order secondary-index candidates. Existing stable
`PreferCandidate` ties keep equality ahead of a range. A constrained rowid
range replaces the table scan when both publish zero work from a zero-row
estimate, so stale or approximate statistics cannot erase the executable
range contract.

An unconstrained secondary-index full scan remains published for ordering and
aggregate analysis, but it is not eligible to replace a constrained rowid
equality or range in normal predicate access selection. Only a secondary
candidate with an equality or range constraint may compete with constrained
rowid access. Otherwise a narrow covering index can win the byte-width cost
while still visiting every source row, moving residual evaluation ahead of
the bounded INTEGER PRIMARY KEY traversal. That contradicts the reference
evaluation phase and leaves the frozen `audit-02500` case incorrect.

Validation reconstructs every range bound from its original predicate and
conjunct position and requires:

- a valid core/source/table/root identity eligible for rowid;
- at least one bound;
- source-independent valid key IDs;
- exactly normalized lower/upper orientation and inclusive flags;
- selected and residual terms that are disjoint and complete;
- candidate metadata/cost equal to the formula above; and
- forward or reverse direction only.

Stable explain text is:

```text
SEARCH "table" USING INTEGER PRIMARY KEY (rowid>? AND rowid<=?)
```

Omit absent constraints, retain logical lower-before-upper operator order, and
append ` REVERSE` after the closing parenthesis for reverse traversal.
`sqlite_schema` uses the same source-name rendering. Reverse physical
traversal never swaps the displayed logical operators.

This slice does not add direct ORDER BY satisfaction for ordinary or compound
queries: their existing sorter decisions remain unchanged, and compound arms
feed their existing materialization/order machinery. A rowid range is ordered
like a bounded table traversal for the existing nonjoined aggregate analysis.
It is not an exact one-row or fixed grouping key. Existing indexed MIN/MAX or
compatible aggregate ordering may select reverse direction after candidate
cost selection; ordinary plans remain forward.

### 3. Extend the existing typed rowid seek

Extend `RowIdSeekMode` to the complete equal, greater-or-equal, greater,
less-or-equal, and less set. Keep the same `SeekRowIdInstruction`, cursor/key/
missing-target operands, verifier state, and B-tree cursor.

For equality, retain the current lossless-rowid rule.

For range modes, reproduce pinned table-seek conversion:

1. apply numeric affinity to TEXT without changing the caller's register;
2. use INTEGER directly;
3. for REAL, obtain SQLite's bounded integer approximation, compare that
   integer exactly with the REAL, and adjust strict/inclusive mode in the same
   two cases as `OP_Seek*`;
4. NULL produces no candidate for every mode;
5. nonnumeric TEXT or BLOB produces no candidate for lower-bound seeks
   (`>`/`>=`) but positions at Last for upper-bound seeks (`<`/`<=`); and
6. map the resulting mode to the existing `BtreeSeekMode`.

Reuse `ApplyAffinity`, `CoerceIntegerForBitwise`, and exact
`CompareSqlIntegerAndReal`; add no second numeric parser or B-tree search.
Cover infinities and signed 64-bit endpoints through the same helpers.

Start positioning is exactly:

| Direction | Logical start | Seek |
|---|---|---|
| forward | lower `>` | greater |
| forward | lower `>=` | greater-or-equal |
| forward | no lower | Rewind |
| reverse | upper `<` | less |
| reverse | upper `<=` | less-or-equal |
| reverse | no upper | Last |

For REAL input, let `i = CoerceIntegerForBitwise(real)` and
`c = CompareSqlIntegerAndReal(i, real)`:

| Exact comparison | Mode adjustment |
|---|---|
| `c > 0` | greater becomes greater-or-equal; less-or-equal becomes less |
| `c < 0` | greater-or-equal becomes greater; less becomes less-or-equal |
| `c == 0` | unchanged |

Greater-or-equal and less are unchanged for `c > 0`; greater and
less-or-equal are unchanged for `c < 0`.

After applying numeric affinity to a TEXT clone:

| Storage class | Range seek behavior |
|---|---|
| INTEGER | use the requested mode directly |
| REAL | use the adjustment matrix above |
| NULL | take the missing edge for every mode |
| nonnumeric TEXT or BLOB with greater/greater-or-equal | take the missing edge |
| nonnumeric TEXT or BLOB with less/less-or-equal | call Last |

The final case must use a real Last operation, not a synthetic maximum-rowid
seek: strict less-than must still include an existing `INT64_MAX` rowid.

### 4. Lower through the canonical table loop

Add one rowid-range producer alongside table scan and equality lookup:

1. initialize LIMIT/OFFSET and statement guards through existing services;
2. open the table cursor;
3. choose the physical start bound from lower/upper according to direction;
4. evaluate that start key once and emit the typed rowid seek, or Rewind/Last
   when no start exists;
5. only after successful positioning, evaluate the opposite logical end key
   once;
6. at the row-loop head, read the current rowid and compare it with the end
   key using numeric affinity and the original inclusive/exclusive semantics;
7. send a true or NULL stop result to the existing completion edge;
8. run unchanged residual filters, OFFSET, projection/consumer, LIMIT, and
   Next/Previous; and
9. close through the existing positioned and pre-open completion paths.

Use the existing expression homes, one existing/shared comparison register
where available, and one rowid scratch only when an end bound exists. Add no
range executor, cursor wrapper, key container, comparator, or VM dependency on
bound expressions.

Start-bound errors occur before end-bound evaluation. End bounds are not
evaluated for an empty or unsatisfied start. A runtime NULL bound yields no
row. Selected bound predicates are removed from residual evaluation; all
unselected predicates retain their original phase and order. This ordering is
the mechanism required to resolve the remaining ADR-0104 audit case.

Loop-head stopping is exactly:

| Direction | Logical end | Stop when |
|---|---|---|
| forward | exclusive upper `<` | rowid `>=` end |
| forward | inclusive upper `<=` | rowid `>` end |
| reverse | exclusive lower `>` | rowid `<=` end |
| reverse | inclusive lower `>=` | rowid `<` end |

Every end check uses NUMERIC affinity and takes the completion edge on true or
NULL.

Define one borrowed `RowIdTraversal` resolver and one shared lowering protocol
for:

- start selection and positioning;
- delayed end-key evaluation;
- loop-head end checking; and
- Next/Previous selection.

Consumer-specific row bodies and completion labels remain, but
`EmitTableScan`, `EmitDistinctTableScan`, `EmitStreamingTableCore`,
`EmitOrderedSelectCore`, `EmitMaterializeSelectRows`, `EmitDemandOneCore`,
grouped access, and `global_aggregate_access.cpp` all call this same protocol.
Indexed MIN/MAX uses it through the same global-aggregate service.

The source cursor is opened before the start expression is evaluated. Bound
values use their expression-home registers. Any physical end allocates one
rowid scratch and one comparison-output register, reusing an already-owned
comparison scratch only when lifetimes are disjoint. Extend the
global-aggregate service/input contract with those exact resources rather
than allocating private module registers.

`SeekRowIdInstruction` verifier flow keeps a positioned fallthrough and an
unpositioned missing edge. The loop-head end edge still has a positioned
cursor and reaches the existing cleanup boundary before merging with
exhaustion. A start-missing or empty path never initializes and never requires
end registers.

### 5. Wire every nonjoined read consumer

Ordinary projection, ORDER BY, LIMIT/OFFSET, DISTINCT, compounds, grouped and
global aggregates, aggregate HAVING transfer, scalar/EXISTS private demand,
and reset/rebind use the same rowid-range producer.

Global MIN/MAX may select rowid range through normal cost/order analysis but
retains its separate aggregate result and empty-input contracts. Grouping may
reuse forward rowid order only through the existing exact grouping proof.
Source-free, joined, automatic-index, and RIGHT/FULL replay paths remain
unchanged.

Do not admit rowid ranges for UPDATE or DELETE in this slice. Their one-pass
mutation cursor, rowid-moving rules, statement rollback, and duplicate-target
contracts require a separate measured design.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before production changes.

Establish the unchanged baseline first with a deterministic SQLite-written
512-byte-page fixture `range_rows(id INTEGER PRIMARY KEY, payload BLOB)`
containing rowids 1 through 128 and a fixed 32-byte payload. Use cache capacity
64 pages, rollback journal, synchronous FULL, mmap zero, temp storage memory,
one warmup, then measured execution.

Run identical parameter schedules in Modern and pinned SQLite:

- empty upper-only: `count(*),coalesce(sum(id),0)` for `id < 0`;
- lower-only selected cardinalities 32/64/128: `id >= 97`, `id >= 65`,
  `id >= 1`;
- upper-only selected cardinalities 32/64/128: `id <= 32`, `id <= 64`,
  `id <= 128`; and
- two-sided selected cardinalities 32/64/128: `[49,80]`, `[33,96]`,
  `[1,128]`, with inclusive parameters.

Record schema/image digest, result count/sum digest, Modern VM instructions,
B-tree comparisons and source-page reads, and pinned
`SQLITE_STMTSTATUS_VM_STEP`. Preserve the generated image and exact command in
session evidence before production.

The fixed-work acceptance is:

- every result/error and digest matches pinned SQLite;
- the empty range performs seek-scale work and uses less than one quarter of
  the unchanged Modern scan baseline instructions and source-page reads;
- B-tree comparisons for positioning remain at most 16 on the 128-row image;
- 32/64/128 runs grow as a fixed seek plus returned-row work, not as an
  additional 128-row pre-scan; and
- no unchanged workload regresses its frozen instruction, comparison,
  page-read, allocation, or deadline contract.

This fixed-work diagnostic is the slice's performance baseline. Existing
uninstrumented native performance gates remain non-target regressions; do not
introduce benchmark instrumentation into the production library.

Then cover:

- lower-only, upper-only, and two-sided bounds;
- all four operators in both operand orientations;
- forward/reverse traversal and rowid ORDER BY satisfaction;
- INTEGER, exact/fractional REAL, numeric/nonnumeric TEXT, BLOB, NULL,
  infinities, and signed 64-bit endpoints;
- parameter reset/rebind and start/end evaluation counts;
- empty table, empty range, start miss, and end-before-start;
- selected-term removal with residual order, including the frozen audit case;
- rowid aliases, hidden rowid spellings, `sqlite_schema`, unary-plus barriers,
  and WITHOUT ROWID fallback;
- ordinary, ordered, DISTINCT/compound, global/grouped aggregate, HAVING,
  scalar/EXISTS, and LIMIT-zero consumers;
- unchanged joined and mutation fallback plans;
- verifier modes, cursor states, OOM, and source-I/O cleanup; and
- exact access cost/order explanations.

Extend the existing separately bounded index-range corpus/oracle with rowid
range cases. Keep every old case and native outcome unchanged, retain its
prepare-error and native-valid unsupported controls, reuse the existing
runner/fixture/profile, and do not raise the primary 512-case corpus limit.

Re-run the frozen predicate audit; this slice targets 20/20 exact matches.
Add no accepted difference. Run the complete fast tier and ADR-0102's selected
compiler, sanitizer, native, performance, fuzz, format, engineering, and graph
gates. Preserve the original sanitizer model's ten-second deadline and all
existing resource ceilings.

Obtain GPT-5.6 Sol implementation and exact-tree review. Require all nine
exact-head hosted checks before guarded squash merge and verified named branch
cleanup. This slice does not complete A4, R2, the direct-bytecode verifier
finding, node 43, or later nodes.

### Pre-production fixed-work baseline

The accepted design was measured before production changes on the exact
SQLite-written image:

- SHA-256
  `e713a4b1415a3d864deac1eac5ead5ed4e7bf9d28738b2e4416b61f90b3fbadc`;
- 6,656 bytes, 512-byte pages, 128 rows, and 32-byte payloads; and
- pinned SQLite source ID
  `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.

Every Modern plan is an unchanged table scan: it records zero B-tree seek
comparisons and reads all 13 source pages.

| Case | Rows | Modern VM instructions | Pinned VM steps |
|---|---:|---:|---:|
| empty `id<0` | 0 | 655 | 17 |
| lower 32/64/128 | 32/64/128 | 750/846/1038 | 142/270/526 |
| upper 32/64/128 | 32/64/128 | 750/846/1038 | 208/400/782 |
| two-sided 32/64/128 | 32/64/128 | 1070/1230/1550 | 209/401/783 |

All count/sum results match. The empty range consumes 63.1% of the 128-row
upper scan's VM instructions and 100% of its source-page reads. The accepted
post-change bounds are fewer than 259 VM instructions, fewer than 3.25
source-page reads, and at most 16 B-tree comparisons for the empty case, plus
the fixed seek-plus-returned-row scaling contract above.

The fixture, commands, per-case counters, and pinned outcomes are preserved in
`rowid-range-fixed-work-baseline.json` in the session evidence.

### Post-implementation fixed-work result

The same image, warmup, bindings, cache, and instrumentation were rerun
against the implementation:

| Case | Rows | Modern VM instructions | B-tree comparisons | Source pages |
|---|---:|---:|---:|---:|
| empty `id<0` | 0 | 19 | 0 | 3 |
| lower 32/64/128 | 32/64/128 | 143/271/527 | 9/9/10 | 5/8/13 |
| upper 32/64/128 | 32/64/128 | 242/466/911 | 0/0/0 | 5/8/13 |
| two-sided 32/64/128 | 32/64/128 | 243/467/912 | 10/9/10 | 5/9/13 |

All count/sum results remain identical to pinned SQLite. The empty range
reduces Modern VM instructions by 97.1% and source-page reads by 76.9% from
the frozen scan baseline. Its 19 instructions, three page reads, and zero
B-tree comparisons pass the accepted limits of 259, 3.25, and 16. The
32/64/128 schedules grow from one bounded positioning operation plus returned
rows and pages; they do not perform an additional 128-row pre-scan.

The exact post-change counters and their before/after pairing are preserved in
`rowid-range-modern-after.tsv` and `rowid-range-fixed-work-after.json` in the
session evidence.

## References

- ADR-0031: Typed Bytecode Programs
- ADR-0035: Deterministic Basic Read Optimization
- ADR-0036: Physical Read Plan Lowering
- ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0063: Global Aggregate Execution
- ADR-0067: Global Aggregate Access Lowering Module
- ADR-0068: SQLite-Aligned Grouped Aggregate Execution
- ADR-0094: NULL-Rejecting Index Range Traversal
- ADR-0102: Change-Aware Verification
- ADR-0104: Conditional Predicate Branch Emission
- `docs/sqlite-reference-audit.md`: remaining A3 access case
