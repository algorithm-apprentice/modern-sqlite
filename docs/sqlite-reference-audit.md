# Implemented-Engine SQLite Reference Audit

- Audit revision: `ebeb974bdc0c935ef627f90b9da076814eee8091`.
- Reference: SQLite 3.54.0, Git
  `cb547ab3e931c7766e24834af5ef6c4578863e3d`, Fossil
  `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
- Date: 2026-10-16.
- Status: Initial whole-implemented-engine review complete; focused
  reproductions and corrections remain in progress.
- Follow-up: A5 is corrected and verified by merged PR #205; other findings
  remain open unless their own delivery evidence states otherwise.
- Traceability: `implement-advanced-sql`, ADR-0002, ADR-0045, ADR-0053.

## Scope and interpretation

Compare the implemented engine, including completed prerequisites and admitted
advanced-SQL slices. Use ADR-0045's reference-faithful storage mechanisms,
observable SQL semantics, and Modern infrastructure distinctions. A different
class, container, physical plan, or opcode is not by itself a defect.

This is a bounded source-and-behavior audit, not a proof that every SQLite
input, crash, allocation boundary, or SQL composition is equivalent. Review
coverage and remaining uncertainty must be reported explicitly. Passing
existing tests does not discharge a newly reproduced discrepancy.

Do not count unimplemented features as admitted-surface bugs. In particular,
WAL, complete multi-connection locking, C API compatibility, extensions,
CTEs, views, triggers, derived-query sources, IN, window functions, CASE,
SQL CAST syntax, and the remaining constraint/conflict surfaces retain their
documented delivery boundaries. Scalar-value conversion primitives do not
imply that SQL CAST syntax has been delivered.

The parked B-tree header-reuse optimization and open binder-foundation PR
#203 are not part of the audit revision. Neither is discarded or treated as a
completed sanitizer repair.

## Method

1. Verify the native source identity and rebuild the exact audited Modern
   revision rather than reusing binaries from a different worktree state.
2. Review actual source mechanisms and cross-layer ownership/error seams,
   with native file/line references.
3. Reuse the existing profile-validated native oracle and Modern read-trace
   protocol. Compare rows, storage classes, exact REAL bits, metadata,
   primary error codes, failure phases, and lifecycle observations.
4. Generate independent bounded cross-feature probes without altering
   committed fixtures, old oracle records, frozen workloads, allocation
   limits, or deadlines.
5. Compare independent mutation traces, public transaction state, final
   native-readable contents, and `PRAGMA integrity_check`.
6. Separate matched behavior, explicit deferrals, confirmed root causes,
   unverified concerns, and performance differences.

The extended SQL probes use private SQLite-created fixtures and the existing
oracle profile, including UTF-8, database settings, compile-option checks,
source hashes, and limits. A second reproduction uses the unchanged joined
index fixture, avoiding dependence on a new diagnostic schema.

### Source coverage

| Area | Reviewed seams and evidence | Disposition |
|---|---|---|
| Bytes, errors, instrumentation | Ownership/result contracts and existing unit/layering gates; no requirement to reproduce SQLite containers | Retain Modern infrastructure; allocation-boundary gaps below remain actionable. |
| Binary coding, database format, record codec | Varints, endian/checksum rules, header normalization, serial widths, signed values, NaN, prefix/index comparison, independent golden vectors | No new valid-record ordering finding; owning decode/error allocation paths need correction. |
| POSIX VFS, cache, PageBitvec | Short I/O, synchronization, delete ordering, stable pins, dirty/LRU state, rekey, adaptive membership and failure state | Zero-size access policy differs; no additional confirmed cache/bit-vector defect. |
| Journal, Pager, transaction coordinator | Publication/playback, hot recovery, savepoints, write ordering, error state, crash/seam evidence | Recovery metadata short reads and implicit-completion state are confirmed concerns. |
| B-tree pages, cursors, writer, diagnostics | Payload/overflow bounds, signed seek, interior traversal, pin lifetime, freelist/balance/root lifecycle, ownership/reachability, bidirectional mutation/crash evidence | No new valid-file balancing/data-loss finding; writable unused-link policy and cursor allocation boundaries differ. |
| Text, values, functions | Conversion, exact mixed comparison, scalar ties, aggregate state/overflow/compensation, collation and embedded NUL | Aggregate numeric TEXT classification and REAL storage-form rules differ. |
| Syntax, catalog, binding | Precedence and explicit boundaries, schema/default/index normalization, namespaces, aliases, captures, aggregate phases, independent SQL and mutation transcripts | Qualified wildcard publication differs; deferred query-copy/outer-aggregate surfaces remain separately scoped. |
| Logical/physical planning and lowering | Source ownership/readiness, access admission/costs, range residual removal, ordering/group destinations, preservation/replay, conditional versus value evaluation | A1-A5 below; typed subroutines/correlated ownership are reused, not replaced. |
| Bytecode, VM, read session | Descriptor/dataflow/capability checks, register/cursor transitions, record-cache invalidation, bind/reset/finalize/reprepare | Direct-bytecode virtual-row/range mismatch reproduced; no current SQL lowering reach or additional confirmed read-session lifecycle defect. |
| Temporary storage | Shared budgets, migration, buffered PMAs, stable run order, bounded fan-in, top-N, relation borrowing, poison/reset/cleanup | No confirmed additional sorter/relation/order/budget defect in the bounded review. |
| Writable SQL and public transaction state | DML snapshots, duplicate targets, constraints, index maintenance, schema/statistics, savepoint outcomes, 98 independent traces | Ordinary traces match; injected implicit commit failure exposes a separate completion seam. |

The three independent source reviewers used GPT-5.6 Sol. They read their
assigned production files in full or function by function and mapped actual
native mechanisms. Test inventories and relevant assertions were reviewed;
not every assertion or every native source line was re-audited. The main
agent directly reviewed SQL compilation seams and executed cross-feature
comparisons. Absence of a confirmed finding is not module-wide equivalence.

## Evidence collected

| Evidence | Result at the audit revision | Limitation |
|---|---|---|
| Complete Debug unit tier | 1,399 cases pass | Existing expectations can themselves encode a native divergence. |
| Remaining Debug tiers | 50 cases pass | Model, differential, CLI, platform, tool, and project coverage is finite. |
| Existing committed read corpora | 697 comparisons and 13 explicit unsupported boundaries pass | The read corpus has reached its 512-case limit; additional domains need a separate corpus rather than a relaxed limit. |
| Isolated native contract tier | 68 cases pass | Existing performance and compatibility inputs remain unchanged. |
| Initial extended SQL matrix | 1,963 transcripts: 1,831 exact matches, 85 expected unsupported cases, 47 admitted differences | Differences are test instances, not independent bug counts. |
| Ordinary nullable-index follow-up | 26 of 102 transcripts differ | All observed differences involve upper-only range NULL exclusion. |
| HAVING follow-up | 4 of 10 transcripts differ | These reduce to the same index-range root cause. |
| Qualified wildcard follow-up | 4 of 32 transcripts differ | Direct qualified columns remain physical; wildcard expansion differs. |
| Mixed three-source join chains | 384 row/group transcripts match | Bounded combinations of INNER/LEFT/RIGHT/FULL, equality/IS/arithmetic keys, NULL filters, and captures. |
| Conditional predicate follow-up | 12 of 20 transcripts differ | Conditional jumps and value-expression evaluation have different native schedules. |
| Independent mutation audit | 98 traces, 281 statements match | Covers selected DML, duplicate targets, constraints, indexed maintenance, schema operations, savepoints, public state, and final native integrity. It is not an exhaustive mutation/crash campaign. |

The extended campaigns contain 2,511 transcripts, including 85 explicit
unimplemented-syntax controls. Their 93 admitted differences are grouped by
root cause below. Related follow-up cases intentionally overlap; these are
not 93 independent defects.

## Confirmed findings

### D1. Short journal metadata can authorize destructive false recovery

Priority: **critical durability risk; correction takes precedence over A1**.

Source review shows that a successful short VFS read of a size-admitted
rollback header becomes end-of-journal rather than I/O failure.
`src/storage/journal/rollback_journal.cpp:105-112,296-321,371-403` collapses
the result into absent playback metadata or a finished stream.
`src/storage/journal/journal.cpp:731-767` can then finalize/delete the journal.
Short super-journal tail/name reads have the same interpretation gap at
`rollback_journal.cpp:138-172`.

If database pages were already written, the journal can be the only source
of the old image. A failed read must not delete it or publish a recovered
snapshot. Native `src/pager.c:1627-1674,2872-2924,3007-3039` propagates
header I/O failure and finalizes only after success, while deliberately
tolerating short page-record crash tails at `:2972-2995`.

The pre-implementation Pager regression dynamically observed false success,
journal deletion, snapshot publication, and failure to restore the old page.
First/later-header and super-journal metadata regressions also failed while
the short page-record control passed. ADR-0095's bounded correction now
passes those regressions, the compiler/sanitizer matrix, the 1,403-case fast
tier, 68 isolated native contracts, and 20,000 read plus 20,000 write
generated inputs. The 27 inherited work properties remain unchanged.
Bounded GPT-5.6 Sol implementation review is approved; final exact-tree and
hosted delivery gates remain open. Physically truncated files and
complete-but-unpublished headers retain their crash-tail behavior.

### D2. Implicit commit rollback leaves stale statement success

Priority: **high observable lifecycle correctness**.

If an implicit DML VM completes but commit fails before durability, the
coordinator can successfully roll back the transaction and invalidate its
statement token. `src/session/write_session.cpp:461-478,600-659` nevertheless
retains pending success/change/catalog state. `changes()` can remain the
previous successful count, and Reset retries completion with the stale
token rather than making the statement reusable.

Native `src/vdbeaux.c:3412-3453,3489-3498` clears changes on autocommit
failure and `src/vdbeapi.c:126-149` rewinds even when reset returns the prior
error. A fix must distinguish this rollback from a durable
`kWriterFinished` cleanup failure, whose existing retry is intentional.
An injected lock-failure reproduction confirms rollback with the inserted
row absent, stale `changes()==1`, and failed reset/re-Step with a stale
transaction token. The pinned native BUSY control reports zero changes,
returns the prior error from Reset, and then successfully re-executes the
same statement. Ordinary successful and constraint traces did not exercise
this seam.

### D3. Empty POSIX super-journal files have the wrong existence meaning

Priority: **high consequence, sidecar-dependent recovery difference**.

`src/platform/posix_vfs.cpp:1817-1844` reports a zero-size regular file as
existing. Native `src/os_unix.c:6867-6898` reports it absent for
`SQLITE_ACCESS_EXISTS`. A retained cold individual journal referencing an
empty super-journal placeholder can therefore replay committed data in
Modern when native skips playback.

Modern does not create attached-database super-journals yet, but recovering
SQLite-created trailers is an admitted ADR-0041 surface. Custom VFS
existence policies and production POSIX parity must not be conflated.
The production-POSIX/default-native-VFS probe confirms `true` versus `false`
for the same zero-byte regular file. The broader cold-journal consequence
remains source-derived; its committed recovery-seam regression is still
required.

### A1. Upper-only index ranges admit SQL NULL keys

Priority: **high, silent wrong results**.

Using the unchanged joined-index fixture:

```sql
SELECT count(*) FROM key_rows WHERE numeric_key<3;
```

SQLite returns `4`; Modern returns `7`. With `<=3`, SQLite returns `5`;
Modern returns `8`. The three extra rows have NULL numeric keys. A scalar
query ordered by the same key returns NULL instead of `1`, and a pushed
HAVING predicate emits an extra NULL group.

`src/optimizer/physical_plan.cpp:1436-1460` selects the range predicate, and
`:3385-3402` removes selected predicates from residual evaluation. However,
`src/lowering/plan_lowering_internal.hpp:85-105` models explicit start/end
bounds and equality prefixes without the implicit non-NULL boundary.

Pinned `src/wherecode.c:1943-2022,2074-2078,2180-2186` explicitly seeks past
initial NULLs or stops before terminal NULLs, depending on the physical index
order and traversal direction. Merely comparing against the upper bound does
not enforce SQL's NULL-rejecting range semantics.

Repair the canonical traversal/key-block mechanism for ordinary covering,
noncovering, grouped/global aggregate, private-query, and joined consumers.
Do not patch only HAVING or reintroduce a private comparator.

### A2. Qualified left wildcards lose native RIGHT/FULL USING values

Priority: **high, wrong values and metadata**.

For a RIGHT/FULL join with `USING(id)` or `USING(t)`, native expansion of
`a.*` can use the merged joined value, even though an explicitly written
`a.id` or `a.t` remains physical. Modern instead publishes the physical
left value for the qualified wildcard, losing unmatched-right keys and, for
NOCASE matches, retaining the wrong spelling. FULL also retains a declared
type where the native merged expression has none.

Modern's `src/binder/bound_select.cpp:4426-4527` restricts joined-value
expansion to unqualified wildcards. Pinned
`src/select.c:6367-6387` deliberately omits a physical qualifier when the
source lies to the left of a RIGHT/FULL USING boundary.

Repair wildcard expansion through the existing joined namespace and
coalesce ownership, without changing direct qualified-column resolution.
Recheck later USING boundaries and mixed chains, not only a two-source join.

### A3. Predicate contexts use eager value-expression evaluation

Priority: **medium, observable error-phase differences**.

For example, all IDs in the diagnostic input are positive:

```sql
SELECT id
FROM audit_l
WHERE id>0 OR abs(-9223372036854775808)
ORDER BY id;
```

SQLite returns the rows; Modern raises integer overflow. Similar differences
occur in HAVING, NOT, and NULL-containing predicates.

`src/lowering/plan_lowering.cpp:5728-5738` first emits the complete expression
as a value. Its binary emitter at `:5499-5510` eagerly evaluates both operands
unless a constant truth hint applies. Native distinguishes eager value
emission from conditional `sqlite3ExprIfTrue`/`sqlite3ExprIfFalse` in
`src/expr.c:6224-6585`.

A repair must preserve SQL three-valued logic and the native error schedule
without turning ordinary SELECT value expressions into universally
short-circuiting expressions.

### A4. Joined ON readiness and access-key phases remain incomplete

Priority: **medium correctness; prerequisite for computed-key admission**.

The original 80-case audit found 16 differences. The expanded count/row and
empty-side matrix found 38 differing transcripts. INNER source-only guards,
constant guards before empty inputs, function keys, and outer lookup misses
do not share one native evaluation phase.

The current producer emits a complete own-ON condition per physical
candidate at `src/lowering/plan_lowering.cpp:9601-9605,9641-9645`.
`ReadLoweringJoin.KeepsComputedJoinKeysInTheirExistingCandidatePhase` at
`tests/unit/lowering/plan_lowering_test.cpp:1985-1995` even expects an overflow
for a source-false guard where pinned SQLite returns count zero.

ADR-0081 correctly warns that determinism alone does not authorize hoisting,
but retaining a candidate scan is not itself proof of full native error
compatibility. Reproduce and correct the schedule through canonical
predicate/access contracts before admitting broader function keys. Do not
add a second guard collection.

### A5. Arithmetic joined keys retained quadratic candidate scans

Priority at audit: **high performance; unchanged sanitizer deadline blocker**.
Status: **corrected by ADR-0096/PR #205**.

At the audited revision, `b.id=a.id+?3` never reaches a joined point candidate
because
`src/optimizer/physical_plan.cpp:481-509` admits only value leaves and lazy
coalesces. Native computes the equality key and emits a rowid seek in
`src/wherecode.c:1743-1770`.

Result-verified growth probes at 32, 64, and 128 rows execute 18,079, 68,575,
and 267,871 Modern VM instructions. Native executes 1,300, 2,196, and 3,988
VM steps. These show different growth, not a cross-engine CPU-speed ratio.
The admitted zero-shift point control is near-linear.

The exact unchanged complete model executes 2,034,538 instructions with 12
source data-page reads, 27,121 cache hits, and 12 misses. Warm arithmetic
queries read zero data pages. The quadratic work therefore persists despite
source-page caching. These counters do not rule out metadata or temporary
file I/O.

The separately preserved compact header-reuse change reduces model CPU by
about 8%; it does not remove this algorithmic amplification or establish an
exact-head hosted sanitizer repair.

The separate computed-rowid correction now uses canonical typed probes and
ON readiness. The unchanged complete model executes 587,276 instructions;
32/64/128-row arithmetic growth executes 2,343/4,359/8,391, rather than the
quadratic baseline above. The native phase and cross-feature comparisons
gain no new differences.

Exact head `b1ddd3bf92cc442bf722e6c3565438c2c2f5d8e6` passed the original
hosted sanitizer model in 6.60 seconds under its unchanged ten-second
deadline on its first CI run. All nine checks passed, and PR #205 normally
squash merged as `75308c5160689864c6855f4715963b30bdb16acb`.
The separate general callback phases in A3/A4 and other audit findings are
not claimed resolved.

### R1. Aggregate numeric TEXT classification crosses embedded NUL

Priority: **high, wrong aggregate storage class and potential overflow phase**.

Exact public traces independently reproduce both directions:

- `sum(?1)` with TEXT hex `31006532` (`1`, NUL, `e2`) is native INTEGER `1`
  but Modern REAL `1.0`.
- TEXT hex `393030373139393235343734303939320078`
  (`9007199254740992`, NUL, `x`) is native REAL but Modern INTEGER.

`src/runtime/function_registry.cpp:248-307` inspects the complete explicit
TEXT view for exponent/decimal syntax before ordinary affinity. Native
`src/vdbe.c:329-365,436-448` uses zero-terminated real parsing followed by
the exact integer-range rule or a successful full-length integer parse.
Simply truncating before NUL is not a complete correction. SUM/TOTAL/AVG
share this conversion and need one canonical helper plus overflow-latch
regressions.

### R2. REAL storage conversion loses native IntReal serial-form rules

Priority: **medium, persistent-representation mechanism difference**.

`src/vm/vm.cpp:190-223,1392-1415` reuses REAL affinity plus lossless rowid
conversion. Native `src/vdbe.c:3623-3638,3700-3753` distinguishes compact
IntReal storage from eight-byte IntReal, which serializes as REAL.
Native `src/vdbemem.c:802-827` also excludes int64 endpoints when
integerizing an existing REAL.

Executed cases include INTEGER `9007199254740993` under REAL storage
affinity and REAL `-9223372036854775808.0`. Modern serial codes are
`[6,6,1]`, versus native `[7,7,1]` for those two values and the compact
integer control. The alternative record passes native `integrity_check`;
both engines return identical public types and exact REAL bits from either
representation. This is a verified mechanism difference, not a demonstrated
read-result defect or proven database corruption.

### B1. Codec and read-cursor allocation boundaries leak exceptions

Priority: **medium, typed-error contract violations**.

Record materialization/error construction at
`src/format/record_codec.cpp:722-726,824-836` and public read-cursor
Open/CopyPayload paths at
`src/storage/btree/cursor.cpp:1004-1009,1093-1108,1121-1157,1258-1273`
allocate outside a translating boundary. A failed vector, payload, metadata,
or implementation allocation can escape as `std::bad_alloc` rather than the
documented typed out-of-memory result.

Existing allocation-free comparison tests do not inject failures into those
owning operations. The requested GPT-5.6 Sol retry now independently
reproduces escaping `std::bad_alloc` for field-error construction, result
vector allocation, owned TEXT/BLOB materialization, both cursor-open
allocation stages, and both payload-copy APIs against the frozen baseline.
These probes do not replace exhaustive cleanup/pin-release and overflow-copy
regressions in the eventual correction.
Contain only expected allocation exceptions, reuse `Error::OutOfMemory`,
and do not add broad catches or success-shaped defaults.

### B2. Writable payload reads consume an unused overflow link

Priority: **medium, malformed-image point-operation parity**.

`src/storage/btree/writer_internal.cpp:5163-5221` validates the final
overflow page's next link even after all logical payload bytes fit on that
page. Native `src/btree.c:5338-5345` and Modern's read cursor at
`src/storage/btree/cursor.cpp:458-478` do not consume that unused link.

This is not evidence that Modern writes invalid chains. A correction must
keep invalid consumed intermediate links as corruption and keep whole-file
diagnostics strict. The source-derived writable/read/native counterexample
has now been executed against the frozen baseline and exact pinned native
runtime: native and Modern read accept the unused final link, while Modern
writable lookup rejects it. All paths reject the consumed intermediate-link
control. No valid-image or Modern-write corruption claim follows from this
malformed-image point-operation difference.

## Direct-bytecode verifier/runtime finding

The verifier's virtual NULL-row/range-check state at
`src/bytecode/program.cpp:2982-2988,3146-3156` disagrees with runtime
`src/vm/vm.cpp:2259-2264`, which clears logical NULL publication on the
range-end edge. An executable
`NullRow -> CheckIndexRange -> ReadField` program verifies, then fails with
`SQLITE_MISUSE` because the B-tree cursor is not positioned. A no-range
control verifies and returns the logical NULL row. No current SQL lowering
path has been established; this is a confirmed bytecode contract finding,
not a demonstrated SQL defect.

## Repair ordering and remaining verification

Correct D1 first because an I/O fault must not destroy recovery authority.
Then prioritize the remaining durable/lifecycle and silent-result defects
according to their reproduced scope. The NULL-range design has passed
GPT-5.6 Sol review but is deliberately not implemented ahead of D1.

Conditional predicate and ON/access scheduling need bounded designs before
arithmetic admission is broadened. Preserve each source-derived versus
executed distinction above as reproductions are completed. No production
claim of universal equivalence follows from the reviewed inventory.

The audit does not advance the DAG, complete node 43, authorize a blind
sanitizer retry, or weaken any existing gate. Every correction remains a
test-first, reviewed, independently validated coherent slice.
