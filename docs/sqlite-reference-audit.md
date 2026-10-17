# Implemented-Engine SQLite Reference Audit

- Audit revision: `ebeb974bdc0c935ef627f90b9da076814eee8091`.
- Reference: SQLite 3.54.0, Git
  `cb547ab3e931c7766e24834af5ef6c4578863e3d`, Fossil
  `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
- Date: 2026-10-16.
- Status: Initial whole-implemented-engine review complete; focused
  reproductions and corrections remain in progress.
- Follow-up: A5, D1, D3, D2, A1, A2, R1, and B1 are corrected and verified by
  merged PRs #205, #204, #206, #207, #208, #209, #210, and #211. ADR-0102 separately
  improves verification efficiency without changing these corrections or
  required gates. ADR-0103 corrects B2's writable point-read boundary in the
  current slice; other findings remain open unless their own evidence states
  otherwise.
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

The parked B-tree header-reuse optimization and binder-foundation PR #203
are not part of the audit revision. The foundation has subsequently merged
after its own combined-tree review and gates. Neither is treated as the
sanitizer repair; that correction is A5.

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
Status: **corrected by ADR-0095/PR #204**.

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
GPT-5.6 Sol approved the implementation and integrated exact tree. The
normally refreshed head passed all nine hosted checks, including the
unchanged original sanitizer model in 6.68 seconds under its ten-second
deadline, and merged as `f1ed8f52355687ecc0493215c34569cb02f0444a`.
Physically truncated files and complete-but-unpublished headers retain their
crash-tail behavior.

### D2. Implicit commit rollback leaves stale statement success

Priority: **high observable lifecycle correctness**.
Status: **corrected by ADR-0098/PR #207**.

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

ADR-0098 now discards pending completion only after authoritative token
retirement, preserving still-valid durable-cleanup retries. Phase-correct
public and allocation regressions failed before the correction and now pass.
The 27 focused cases and complete Debug suite pass. Seven paired native
lifecycle transcripts also match changes, rowid, autocommit, rows/schema,
reset/finalize errors, and actual reexecution; native accepts all final
Modern images and all fourteen integrity checks pass. All local gates and
nine first-head hosted checks passed, including the unchanged original model
in 6.33 seconds under its ten-second deadline. Merged commit
`27e8a29a9e26ebde29f962d8745ee6069690ef87` has the exact Sol-accepted
tree `24c7054492585d8a3c00c148197bd4b7e34154c3`.

### D3. Empty POSIX super-journal files have the wrong existence meaning

Priority: **high consequence, sidecar-dependent recovery difference**.
Status: **corrected by ADR-0097/PR #206**.

`src/platform/posix_vfs.cpp:1817-1844` reports a zero-size regular file as
existing. Native `src/os_unix.c:6867-6898` reports it absent for
`SQLITE_ACCESS_EXISTS`. A retained cold individual journal referencing an
empty super-journal placeholder can therefore replay committed data in
Modern when native skips playback.

Modern does not create attached-database super-journals yet, but recovering
SQLite-created trailers is an admitted ADR-0041 surface. Custom VFS
existence policies and production POSIX parity must not be conflated.
The production-POSIX/default-native-VFS probe confirms `true` versus `false`
for the same zero-byte regular file. The independent cold-journal/image
comparison also confirms the consequence: nonempty masters restore `[1]`
in both engines, missing masters retain `[1,2]` in both, and empty masters
restore `[1]` in Modern but retain `[1,2]` in native. Native reads the final
Modern image in each case.

ADR-0097 corrects only canonical POSIX `kExists`, preserving custom-VFS
policies and the existing recovery path. Focused platform and real-Pager
regressions failed before that change and now pass, including exact complete
recovery images, missing/nonempty controls, individual-journal finalization,
symlinks, nonregular files, and explicit unexpected errors. The independent
comparison now matches all three native outcomes with byte-identical final
images and native `integrity_check=ok`. The first hosted head also exposed
shared recovery-test directory collisions; atomic reservation in the existing
helper passed 240 two-worker executions across six configurations without
weakening scheduling or deadlines. The normally appended head passed all nine
hosted checks, including the original model in 2.77 seconds under its
ten-second deadline. Merged commit
`b6e5e29904e2a769ae13b9f9de2bc8e2f5566f3d` has the exact Sol-accepted
tree `fd6d37552664b798197fc01b3cc4b7b53d978de8`.

### A1. Upper-only index ranges admit SQL NULL keys

Priority: **high, silent wrong results**.
Status: **corrected by resumed ADR-0094/PR #208**.

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

ADR-0094 now implements that shared borrowed NULL boundary and key emission.
The pre-production fixture red observed `4` versus `7`; all 35 new native
shard cases now pass without changing old records, fixtures, profiles, or
the 512-case cap. Current Debug passes 1,480 tests. An exact replay of all
2,511 frozen audit instances resolves 31 old differences and introduces no
new or changed-unresolved outcome; native replay is unchanged. All local and
nine first-head hosted gates passed, including the unchanged original model
in 4.18 seconds under its ten-second deadline. Merged commit
`b1d60e4dc7324f79aff3165c4830735e8f473e1b` has the exact Sol-accepted
tree `dab91619de35f297b28f81103d46a6171c1d86e9`.

### A2. Qualified left wildcards lose native RIGHT/FULL USING values

Priority: **high, wrong values and metadata**.
Status: **corrected by ADR-0099**, including exact wildcard name spelling.

For a RIGHT/FULL join with `USING(id)` or `USING(t)`, native expansion of
`a.*` can use the merged joined value, even though an explicitly written
`a.id` or `a.t` remains physical. Modern instead publishes the physical
left value for the qualified wildcard, losing unmatched-right keys and, for
NOCASE matches, retaining the wrong spelling. FULL also retains a declared
type where the native merged expression has none.

At the audited revision, `src/binder/bound_select.cpp:4426-4527` restricts
joined-value expansion to unqualified wildcards. Pinned
`src/select.c:6015-6027,6358-6379` deliberately omits a physical qualifier
when the source lies to the left of a RIGHT/FULL boundary and the name
participates in any later USING clause.

Repair wildcard expansion through the existing joined namespace and
coalesce ownership, without changing direct qualified-column resolution.
Recheck later USING boundaries and mixed chains, not only a two-source join.

The bounded follow-up has 197 native/current instances, including different
column spelling and affinity. Its 53 differences include later mixed
boundaries and a coupled unqualified-RIGHT wildcard name discrepancy.
Wildcard names must keep their owning declared spelling even when a resolved
RIGHT value has another source's metadata. The original four cases are not
the complete correction scope.

ADR-0099 now selects generated qualification through the current core's
existing source order and normalized USING links, then reuses canonical
name resolution and provenance checks. Explicit qualified columns remain
physical. The existing metadata builder keeps the wildcard owner's declared
spelling while deriving type, affinity, and collation from the resolved value.
All 197 exact follow-up instances now match native, resolving all 53 former
differences. The frozen 2,511-instance replay against the post-A1 baseline
resolves 12 further instances with no new or changed-unresolved outcome and
unchanged native contracts; these overlapping counts are not added together.

The permanent 48-case namespace shard includes 47 comparisons and one
explicit unsupported control, including a native prepare-error control for
an unrelated later same-name source. A small independent native metadata
fixture records unequal name spelling and declared types; old fixtures,
oracles, profiles, and the 512-case limit remain unchanged. Public and binder
regressions failed before implementation. Complete Debug passes 1,485 tests,
including source-read and real spill-write recovery and the expanded
exhaustive allocation sweeps under the unchanged 1,024-allocation ceiling.
GPT-5.6 Sol accepted the design and bounded implementation without blockers;
the integrated exact-tree and delivery gates remain mandatory.
The accepted exact tree `439285f23cf7c8030726bf692e64717c0e94e678`
passed every local gate and all nine first-head hosted checks, including the
unchanged original model in 4.58 seconds under its ten-second deadline.
PR #209 squash merged as `b692920e590e2f9eebd25dff7e775eb379fb84c4`;
merged main and only the confirmed merged branch were verified before cleanup.

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
Status: **corrected by ADR-0100**.

Exact public traces independently reproduce both directions:

- `sum(?1)` with TEXT hex `31006532` (`1`, NUL, `e2`) is native INTEGER `1`
  but Modern REAL `1.0`.
- TEXT hex `393030373139393235343734303939320078`
  (`9007199254740992`, NUL, `x`) is native REAL but Modern INTEGER.

At the audited revision, `src/runtime/function_registry.cpp:248-307`
inspects the complete explicit TEXT view for exponent/decimal syntax before
ordinary affinity. Native
`src/vdbe.c:329-365,436-448` uses zero-terminated real parsing followed by
the exact integer-range rule or a successful full-length integer parse.
Simply truncating before NUL is not a complete correction. SUM/TOTAL/AVG
share this conversion and need one canonical helper plus overflow-latch
regressions.

The post-A2 bounded baseline compares 168 instances on the unchanged core
fixture and profile: 20 of 91 TEXT cases and six of 36 overflow/lifecycle
cases differ, while all 36 BLOB and five scalar controls match.

ADR-0100 now reuses the shared value scanner, full-length integer parser,
exactness predicates, and REAL conversion through one function numeric-type
operation. Ordinary affinity, CAST, arithmetic, and the numeric state remain
separate and unchanged. All 168 exact instances now match native, resolving
the 26 former differences with unchanged native outcomes. All 2,511 frozen
post-A2 cross-feature outcomes remain unchanged; that earlier population did
not reproduce these parameter-driven NUL cases.

The standard numeric-type shard adds 54 cases, including 53 comparisons and
one unsupported control. It covers both required prepare-control kinds,
types and exact REAL bits, full-length and 51-bit boundaries, error/reset
phases, grouped/modifier/compound/private consumers, and cross-type rebinding.
It reuses the unchanged core fixture, profile, protocol, and limits, and
retains every old corpus and oracle byte. The existing lower/BLOB-to-TEXT
path provides a source-free raw seed without new syntax or generator changes.

Focused aggregate and public-session regressions failed before production
changes. Current Debug passes 1,492 tests with every inherited name retained.
The focused 25 cases and complete 1,432-case fast unit tier pass. The existing
allocation helper retains its original numeric case and exact two-allocation
contract, adding long NUL TEXT paths and successful same-state reset/reuse
after each injected failure. GPT-5.6 Sol accepted design and implementation
without blockers; integrated exact-tree and delivery gates remain mandatory.
The exact accepted tree `61eff1a3246c60b086285f031ceba09e72759271`
passed all unchanged-input local gates and nine first-head hosted checks,
including the original model in 7.27 seconds under its ten-second deadline.
PR #210 squash merged as `ceeb3e8006afad3322698a93ff7022e619b1f06e`;
merged main, parent, trailer, and named merged-branch cleanup were verified.

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
Status: **corrected by ADR-0101**.

At the audited revision, record materialization/error construction at
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

The main-agent current-tree reproduction after PR #210 confirms all nine
named allocation sites still throw. The existing scoped diagnostic was
recompiled against that exact library rather than reused as a stale binary.
The inventory also identifies coupled page/cursor diagnostic constructors
that own strings before a protected boundary. ADR-0101 reuses the codec's
nonthrowing error pattern, explicit ownership boundaries, and the existing
module allocation helpers; it does not broaden allocator handling in the
optimizer/lowering or change persistent validation.
Design review additionally identified 15 page-local lvalue error forwards
that can copy owning diagnostics after the factory. The correction requires
move propagation from non-const Results and nested failure-index coverage,
not only a fail-index-zero factory test.

ADR-0101 now contains expected allocation failures through the existing
nonthrowing error pattern and ownership boundaries. All 15 page errors move
from non-const Results. The shared cursor payload-copy operation retains one
exact buffer, feasibility-before-allocation, the same walker, and unchanged
position/borrows on failure. Public API shape and persistent behavior remain
unchanged.

The existing allocation helpers observed the field and three mixed-record
escapes plus 38 overlapping cursor/page observations before production
changes. Their expanded green controls exhaust ownership and nested
diagnostic allocations, include indices after zero, verify cleanup and
same-input/Pager retry, and prove failed-Open pin release through `EndRead`.
Local and overflow copies cover both cursor types. A real third-read I/O
failure after copy allocation preserves the original error, cursor position,
local bytes, and successful exact retry.

The focused and complete 1,432-case fast tiers passed before and after the
green-only copy consolidation. Current Debug passes 1,493 tests, retaining
every inherited name plus one I/O regression. All 2,511 frozen post-R1 native
and Modern outcomes remain unchanged. Old comparison repetitions, allocation
ceilings, fixtures, oracles, profiles, workloads, and resource controls are
unchanged. GPT-5.6 Sol accepted the amended design and implementation without
remaining blockers; integrated exact-tree and delivery gates remain mandatory.
All unchanged-input local conditions and nine first-head hosted checks
passed on the accepted tree `67bee264a726fbf8d3254b4933e21409f2a3e054`.
The original unchanged model passed in 2.52 seconds under its ten-second
deadline. PR #211 merged as `fd693a8d45d192e92ed888670d3e76e7a980dbbf`;
main, parent, trailer, and named merged-branch cleanup were verified.

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

ADR-0103 corrects the canonical writable payload reader by decoding the next
link only when more logical payload bytes remain. Public encoded-index and
table-mutation operations cover one and multiple overflow pages, exact and
missing keys, repeated access, typed allocation and I/O failures, rollback
state, and consumed-link controls. The pinned runtime accepts all five tested
unused final values and rejects the consumed invalid control. Strict decoding
and whole-file diagnostics remain unchanged.

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

D1, A5, D3, D2, A1, A2, R1, and B1 have merged with exact-head hosted proof,
and the preserved binder foundation is integrated. The user-requested
ADR-0102 verification-efficiency slice merged in PR #212 as
`356bbca5a51d6e3d539bb25fc7f6154e0d49d129`, accepted tree
`e120b0543f6f9f184ffb36e69b72e3522f023b76`. All nine first-head hosted checks
passed; the unchanged original sanitizer model passed in 6.45 seconds under
its ten-second deadline. Main, parent, trailer, and named branch cleanup were
verified. ADR-0103 corrects B2's writable payload boundary in this slice;
exact-tree review and hosted delivery gates remain required.
Prioritize the remaining durable/lifecycle
and silent-result defects according to their reproduced scope. The earlier
NULL-range design is now resumed under ADR-0094; the independent page-view
optimization remains preserved and is not treated as a delivered correction.

Conditional predicate and ON/access scheduling need bounded designs before
arithmetic admission is broadened. Preserve each source-derived versus
executed distinction above as reproductions are completed. No production
claim of universal equivalence follows from the reviewed inventory.

The audit does not advance the DAG, complete node 43, authorize a blind
sanitizer retry, or weaken any existing gate. Every correction remains a
test-first, reviewed, independently validated coherent slice.
