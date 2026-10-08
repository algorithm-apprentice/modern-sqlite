# ADR-0050: Pinned Write Performance Baseline

- Status: Accepted
- Date: 2026-10-08

## Context

ADR-0008 requires matched configuration, deterministic work, correctness
verification, and a committed baseline before write optimization. ADR-0039
provides the corresponding read-only process, provenance, report, diagnostic,
and CI contracts. Nodes 33 through 39 now provide:

- rollback-journal DELETE mode;
- a writable Pager and reference-faithful B-tree mutation;
- implicit and explicit transactions plus named savepoints;
- public `WriteSession` DDL and DML execution;
- deterministic model, fuzz, crash, and pinned-SQLite interoperability
  evidence; and
- fixed-work lower-layer diagnostics for B-tree and transaction operations.

The `establish-write-performance-baseline` node must deliver:

> Matched insert, update, delete, commit, rollback, allocation, I/O, and
> work-count baselines.

Its acceptance criterion is:

> Reports separate default and matched durability configurations and verifies
> final contents.

Existing write timing evidence is intentionally non-authoritative:

- `benchmarks/btree-write-node36-baseline.md` records deterministic structure
  and model results but no matched SQL timing;
- `benchmarks/transaction-node37-baseline.md` records coordinator work and
  allocation counts but no SQLite timing;
- ADR-0048 records lowering allocation budgets and specifies the SQL work
  that a future baseline must expose; and
- ADR-0049 proves correctness under sanitizer, fuzz, crash, and
  interoperability workloads but explicitly excludes timing thresholds.

Pinned SQLite 3.54.0 provides useful precedents:

- `test/speedtest1.c:32-49` makes cache, journal, transaction, and synchronous
  policy explicit;
- `test/speedtest1.c:792-1018` separates ordered and unordered batched INSERT
  work;
- `test/speedtest1.c:1022-1119` measures refill, point UPDATE, scan UPDATE,
  point DELETE, and scan DELETE;
- `test/speedtest1.c:2628-2734` reuses prepared rowid DML inside explicit
  transactions; and
- `test/speedtest1.c:3301-3321` applies cache, synchronous, and journal
  settings independently.

Those tests are reference patterns, not the Modern baseline runner. They mix
unsupported indexes, triggers, and broader SQL; they do not preserve every
raw repetition and final image; and they do not distinguish engine-default
durability from an explicitly matched profile.

Write benchmarking differs materially from read benchmarking:

- every measured repetition changes its database;
- commit and rollback I/O are part of the target work;
- a later repetition cannot reuse the previous repetition's image without
  changing work;
- final contents and recovery sidecars must be checked after each
  repetition; and
- default connection settings may be useful product evidence even when they
  are not fair comparison settings.

The baseline must therefore start every warmup and measured repetition from a
fresh canonical database copy. Copying, opening, configuration, preparation,
closing, integrity checking, and final-content verification remain outside
the measured interval.

This node establishes evidence only. It does not optimize production code,
change the public cache or durability API, relax statement atomicity, or
remove correctness checks to improve a number.

## Decision

### 1. Add a versioned public-session write baseline

Add:

```text
benchmarks/
  write-baseline-v1/
  write_performance.cpp
  write_performance_build_config.hpp.in
docs/
  performance/
    write-baseline.md
tests/
  fixtures/
    write_performance/
      README.md
      schema.sql
      populated.sql
      schema.db
      populated.db
  performance/
    write-workloads-v1.json
    write_benchmark_cli_test.py
    write_fixture_regeneration_test.py
    write_performance_tool_test.py
tools/
  write_performance.py
```

The production library gains no benchmark, SQLite, Python, JSON, timing, or
profiling dependency.

The canonical baseline directory is `benchmarks/write-baseline-v1/`. A
future workload or report semantics change creates a new versioned path
rather than changing version 1 in place.

### 2. Compare two durability profiles without conflating them

Every workload runs under two named profiles.

#### `engine-default`

Modern uses the public `WriteSession` defaults.

SQLite uses the pinned build's connection defaults after opening the
canonical fixture. The runner reads back and records page size, cache size,
journal mode, synchronous mode, mmap, temporary storage, locking mode, and
thread mode. It fails if the effective values cannot be queried.

This profile answers:

> What cost does an application observe when it opens each engine without
> tuning durability?

Its ratios are retained but are informational because effective defaults may
differ. No cross-engine severe-regression threshold is applied.

#### `matched-durable`

Modern still uses its public defaults. SQLite is explicitly configured and
verified to match the current Modern writable contract:

- 4096-byte database pages;
- 512 cached pages;
- rollback journal `DELETE`;
- `synchronous=FULL`;
- memory mapping disabled;
- `temp_store=MEMORY`;
- normal locking mode;
- one connection and one thread; and
- no WAL or shared-memory sidecar.

This profile is the authoritative comparison. Its wall and CPU ratios are
subject to the severe-regression guard.

The baseline does not add a Modern `synchronous=OFF`, cache-size, page-size,
or journal-mode public option solely for benchmarking. Such API work requires
a product requirement and separate decision.

### 3. Reuse the pinned build and preserve read-baseline provenance

The write targets use the same pinned SQLite version, source ID,
`sqlite3.c`/`sqlite3.h` hashes, semantic compile options, C/C++ compilers,
Release configuration, and benchmark static/shared libraries as ADR-0039.

CMake adds:

- `modern_sqlite_write_benchmark`, linked to the ordinary uninstrumented
  engine and pinned SQLite;
- `modern_sqlite_write_diagnostics`, linked to the separately compiled
  instrumented engine and pinned SQLite; and
- one generated write build-configuration header.

The existing instrumented engine target is renamed from the read-specific
name to canonical `modern_sqlite_diagnostic_engine` and shared by read and
write diagnostics. No compatibility alias remains.

`tools/write_performance.py` is a separate standard-library-only command.
It may import a reviewed explicit subset of process, hashing, host-provenance,
and bounded-subprocess helpers from `tools/read_performance.py`, but it does
not change the read tool or its committed hash. Write workload and report
validation remain write-specific.

Generation rejects:

- non-Release or unoptimized targets;
- sanitizer, coverage, profiler, ordinary instrumentation, or LTO flags in
  the timing executable;
- a stale configured Git revision or tree;
- a dirty worktree;
- the wrong repository root, CMake home directory, generator, or target
  compile command;
- the wrong SQLite identity or compile profile; and
- any output path that already exists or aliases an input.

### 4. Pin one schema fixture and one populated fixture

Both fixtures use:

```sql
CREATE TABLE kv(
  k INTEGER PRIMARY KEY,
  v BLOB NOT NULL,
  version INTEGER NOT NULL
);
```

The schema fixture contains no rows.

The populated fixture contains 65,536 rows. Logical row number `n` starts at
one:

- `k = n`;
- `version = 0`; and
- `v` is a 256-byte BLOB containing eight lowercase hexadecimal ASCII digits
  for `n`, followed by 248 ASCII zero bytes.

Both fixtures use:

- 4096-byte pages;
- UTF-8;
- rollback-journal DELETE mode;
- `synchronous=FULL` during generation;
- no auto-vacuum;
- application ID 0 and user version 1; and
- no index, trigger, view, or extra user object.

The manifest pins:

- fixture and SQL hashes;
- byte size, page count, row count, and schema SQL;
- full ordered-content digest;
- expected page-size and cache-pressure classification; and
- absence of journal, WAL, and shared-memory sidecars.

The populated fixture is intentionally larger than the 512-page engine cache.
Write workloads therefore exercise page replacement and spill behavior rather
than fitting entirely in cache.

Fixture regeneration uses pinned SQLite and refuses any integrity, schema,
encoding, page-size, row-value, page-count, hash, or sidecar mismatch.

### 5. Define an immutable nine-case workload matrix

Version 1 contains these cases:

| Case | Initial image | Timed work | Primary unit |
|---|---|---|---|
| `create-table-implicit` | zero-byte file | create 256 distinct rowid tables | statement |
| `insert-point-implicit` | schema | 512 prepared single-row INSERTs, each implicitly committed | row |
| `insert-batch-explicit` | schema | BEGIN, 65,536 prepared INSERTs, COMMIT | row |
| `update-point-implicit` | populated | 512 prepared exact-rowid UPDATEs, each implicitly committed | row |
| `update-scan-implicit` | populated | one scan UPDATE of all 65,536 rows and implicit commit | row |
| `delete-point-implicit` | populated | 512 prepared exact-rowid DELETEs, each implicitly committed | row |
| `delete-scan-implicit` | populated | one scan DELETE of all 65,536 rows and implicit commit | row |
| `mixed-batch-commit` | populated | BEGIN, 4,096 UPDATEs, 4,096 DELETEs, 4,096 INSERTs, COMMIT | row mutation |
| `mixed-batch-rollback` | populated | the same mixed batch followed by ROLLBACK | row mutation |

Each ID, SQL byte string, binding order, key order, value generator, row
count, transaction boundary, and final-state rule is immutable within
workload-semantics version 1.

Prepared DML SQL is:

```sql
INSERT INTO kv(k,v,version) VALUES(?1,?2,0)
UPDATE kv SET v=?1,version=version+1 WHERE k=?2
UPDATE kv SET v=?1,version=version+1 WHERE k>=1
DELETE FROM kv WHERE k=?1
DELETE FROM kv WHERE k>=1
```

CREATE uses deterministic names `t000` through `t255` and schema:

```sql
CREATE TABLE tNNN(
  id INTEGER PRIMARY KEY,
  v BLOB NOT NULL DEFAULT x''
)
```

The exact-update and exact-delete key order uses SplitMix64 plus
rejection-sampled Fisher-Yates with fixed seed
`0xd1b54a32d192ed03`. The manifest records the algorithm and seed. Mixed
commit and rollback consume the first third of the permutation for UPDATE,
the second third for DELETE, and new ascending rowids for INSERT, so every
requested mutation has an unambiguous expected change count.

The manifest records expected values for warmup, measured, diagnostic, and
final verification phases:

- transactions begun, committed, and rolled back;
- statements prepared, stepped, reset, and finalized;
- operations and primary units;
- rows requested and changed;
- bytes bound and final bytes observed;
- sum of per-statement `changes`;
- final `last_insert_rowid`;
- final row count and ordered-content digest; and
- schema object count and schema-cookie relationship.

### 6. Give every repetition a fresh database and connection

One timed engine, profile, and case run in each child process. The opposite
engine may be initialized only after all timers have stopped, every timed
statement and connection has closed, and final verification begins. It never
shares allocator, cache, or connection state with the measured interval.

Each child performs:

1. one complete untimed verification of the canonical input;
2. one untimed warmup on a disposable fresh copy;
3. three measured repetitions, each on a separate fresh copy and connection;
4. final verification of every measured output outside the timer; and
5. strict cleanup of every connection, statement, journal, and temporary
   path.

For a measured repetition:

1. copy or create the initial database outside the timer;
2. open and configure the selected engine outside the timer;
3. prepare reusable DML statements outside the timer;
4. start wall and process-CPU timers;
5. execute the exact case, including required BEGIN/COMMIT/ROLLBACK;
6. stop timers only after the final successful `Step()` or transaction
   command returns;
7. finalize statements and close the connection outside the timer;
8. verify final contents and integrity; and
9. remove the exact owned database and sidecars.

Binding, stepping, result/error checking, reset, transaction commands, journal
I/O, database I/O, sync, and journal deletion are timed. Database copying,
open, PRAGMA configuration, preparation, close, integrity check, and final
scan are not.

CREATE preparation is timed because each statement has distinct SQL and
cannot reuse one prepared program. Its finalization remains outside the timer
only after the statement has completed successfully.

Every measured repetition must last at least 20 ms. A shorter duration is a
harness failure; the runner never calibrates work after seeing elapsed time.

A pre-baseline fixed-work probe on the selected macOS generation host measured
the pinned SQLite cases at approximately 25--101 ms per repetition. Raising
every write case to 200 ms would require roughly one million rows for the
single-transaction INSERT or would change the pinned one-scan DELETE semantics.
The 20 ms admission floor therefore preserves the immutable work while the
three paired rounds and three retained repetitions provide nine samples per
engine, profile, and case. The 10x guard is intentionally much wider than
ordinary sub-100-ms timing noise.

### 7. Match public API lifecycle and SQL semantics

Modern uses `WriteSession`.

SQLite uses one `sqlite3*` connection with `SQLITE_OPEN_READWRITE |
SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX` and prepared statements through
`sqlite3_prepare_v3(..., SQLITE_PREPARE_PERSISTENT, ...)`.

For reusable DML, both engines:

- bind one-based typed values;
- step to `DONE`;
- verify the primary result immediately;
- read the statement change count from the connection;
- reset after every operation, including the final operation, before timers
  stop; and
- retain bindings only when both APIs do so explicitly.

The explicit cases time BEGIN and COMMIT or ROLLBACK through each public SQL
API. They do not call Pager, transaction, B-tree, or VM internals.

After each successful INSERT, the runner verifies the expected
last-insert-rowid. UPDATE, DELETE, CREATE, COMMIT, and ROLLBACK must not
silently replace it.

The mixed rollback case requires:

- successful execution of every DML statement;
- rollback as the timed terminal operation;
- original logical contents after close/reopen;
- byte-identical main database bytes to the initial copy; and
- no surviving journal.

### 8. Verify final contents in both directions

Timing success is necessary but insufficient.

After every Modern repetition:

- reopen the output through a fresh Modern `WriteSession`;
- verify schema visibility, row count, exact ordered values, and storage
  classes;
- open the same image through pinned SQLite;
- require `PRAGMA integrity_check='ok'`; and
- verify the same final logical digest, schema rows, schema cookie, page
  count, and freelist count.

After every SQLite repetition:

- run SQLite integrity checking;
- verify the expected SQLite final state;
- reopen the image through Modern `WriteSession`; and
- verify the same supported tables and exact ordered contents.

Committed outputs from the two engines are not required to be byte-identical.
They must be valid SQLite files with identical logical results. Rollback
outputs are required to be byte-identical to their own initial copy.

No repetition may leave `-journal`, `-wal`, or `-shm`.

### 9. Use paired rounds and retain every sample

The Python runner executes three paired rounds for each durability profile.

Round 0 uses canonical case order and Modern then SQLite.
Round 1 uses reverse case order and SQLite then Modern.
Round 2 repeats canonical order and Modern then SQLite.

Each child retains three measured repetitions. The aggregate therefore
retains nine wall and nine CPU samples per profile, engine, and case.

Reports compute:

- each round's median;
- the all-nine median;
- exact rational Modern-to-SQLite wall and CPU ratios;
- validated decimal renderings;
- minimum and maximum paired-round ratios; and
- engine-default and matched-durable results as separate profile groups.

No failed, interrupted, timed-out, malformed, or unfavorable repetition may
be omitted or replaced. No aggregate is described as a request percentile.

### 10. Version and validate all evidence

`benchmarks/write-baseline-v1/` contains:

- `aggregate.json`;
- `run-manifest.json`;
- every raw timing report;
- every raw diagnostic report; and
- bounded stderr for every child.

Schemas reject unknown fields, duplicate JSON keys, NaN, infinity, booleans
where integers are required, missing repetitions, duplicate indexes,
inconsistent derived values, nonpositive wall time, negative CPU time,
unknown profiles/cases/counters, and missing or extra artifacts.

Provenance includes all ADR-0039 build, source, SQLite, compiler, host,
executable, fixture, workload, command, process, and timer fields plus:

- durability profile;
- initial and final database hashes;
- final logical digest;
- initial/final page count, freelist count, and schema cookie;
- journal/database sidecar cleanup;
- transaction, statement, and changed-row completion counts; and
- per-repetition final verification engine results.

Raw durations and counts are integers. Ratios use exact
numerator/denominator pairs plus validated decimal strings.

The runner bounds stdout, stderr, child duration, file count, database-copy
count, and artifact sizes. It terminates the exact owned process group on
timeout and preserves failure artifacts.

Generation requires a clean committed tree and an absent output directory.
Validation never reruns timing.

### 11. Separate authoritative timing from diagnostics

The timing target links the ordinary uninstrumented engine and has no global
allocation override.

The diagnostic target links `modern_sqlite_diagnostic_engine`, enables the
existing stable Modern counters, and applies a diagnostic-only allocation
override.

Modern diagnostic output records:

- allocations;
- bytes copied;
- VFS calls;
- pages read and written;
- cache hits and misses;
- B-tree comparisons;
- VM instructions; and
- planner work.

The diagnostic target additionally wraps `PosixVfs` in a verification-only
counting VFS passed through `WriteSession::Open()`. The authoritative timing
target uses ordinary `PosixVfs` directly. The diagnostic wrapper records
separately for main database, main journal, temporary subjournal, and WAL
kinds:

- open and close count;
- read and write calls and bytes;
- sync count;
- truncate count;
- delete count and directory-sync request;
- lock and unlock count;
- access and full-path queries; and
- random-byte calls.

The wrapper delegates every operation unchanged and performs no buffering,
retry, timing, or failure suppression.

SQLite diagnostics retain native names from `sqlite3_db_status`,
`sqlite3_stmt_status`, `sqlite3_status64`, `sqlite3_changes64`, and
`sqlite3_total_changes64`, including cache hits/misses/writes, cache bytes,
VM steps, full-scan steps, statement runs, reprepares, and memory high-water
values.

SQLite native counters are not renamed as Modern counters when semantics
differ. The report also includes engine-neutral transaction, statement,
operation, changed-row, final-row, byte, and digest counts.

Instrumented elapsed time is diagnostic only and cannot populate baseline
timing or satisfy a guard.

### 12. Apply the severe-regression guard only to matched durability

The first write baseline uses ADR-0008's broad guard for
`matched-durable`.

Every matched case must satisfy:

- aggregate wall ratio at most 10.0;
- aggregate process-CPU ratio at most 10.0;
- every paired-round wall ratio at most 10.0; and
- every paired-round CPU ratio at most 10.0.

`engine-default` ratios are always retained but do not pass or fail the
cross-engine guard.

Correctness, completion, duration, and report-schema failures fail both
profiles.

A validated guard failure writes the complete structurally valid report and
returns exit code 2. Harness, environment, provenance, correctness, process,
or malformed-input failures return exit code 1.

### 13. Keep hosted CI deterministic and bounded

Hosted CI:

- validates the committed write baseline and every raw artifact;
- validates fixture hashes and final-state fingerprints;
- verifies the severe-regression result already recorded;
- builds timing and diagnostic targets from pinned SQLite; and
- runs reduced smoke work for every profile, engine, and case.

CI does not regenerate or gate on fresh hosted-runner timing ratios.

The existing pinned performance-contract job is extended rather than adding
another full configure/build job. Its 20-minute job timeout remains the hard
upper bound.

Smoke mode performs:

- one CREATE statement;
- one implicit point DML statement;
- one explicit batch of eight rows;
- one scan DML over eight rows; and
- one mixed commit or rollback over eight row mutations.

Smoke validates identity, profile application, statement lifecycle, final
contents, integrity, sidecar cleanup, raw report schema, and exit codes.
Smoke elapsed values are never admitted as baseline evidence.

### 14. Provide fixed selected-case profiling

The timing binary supports:

```text
profile ENGINE PROFILE CASE DATABASE WORK
```

The replay:

- copies or creates the selected initial image;
- opens and configures one engine;
- prepares and warms outside the profile interval;
- executes exactly the requested fixed work;
- verifies final contents and cleanup; and
- emits the work digest.

The guide documents Xcode Time Profiler, Linux `perf stat`, `perf record`,
optional Cachegrind, and diagnostic-counter replay.

Profile timing is never baseline evidence. Unsupported tools or capture modes
fail explicitly.

### 15. Use strict exit codes and TDD slices

The write tool and executables reserve:

- `0` for complete validated success;
- `1` for usage, environment, provenance, process, timeout, malformed data,
  or harness failure; and
- `2` for correctness or validated matched-profile guard failure.

Implementation order is:

1. add strict workload/report/profile/exit-code tests and record the absent
   tool failure;
2. add fixture-regeneration tests and record the absent generator failure;
3. add C++ smoke tests and record absent benchmark targets;
4. implement fixture generation and strict manifest validation;
5. implement one matched INSERT case end to end;
6. add remaining INSERT, UPDATE, DELETE, commit, rollback, and CREATE cases;
7. add engine-default profile reporting;
8. add Modern counting-VFS and SQLite native diagnostics;
9. add selected-case profile mode and documentation;
10. run the complete validation matrix;
11. generate the baseline twice with identical work/provenance coverage and
    independent timing samples; and
12. receive design and implementation review with no unresolved issue.

Each implementation slice begins with a focused failing test. A discovered
engine correctness defect is fixed in its owning production module before a
benchmark expectation is accepted.

## Pre-Implementation Proof Obligations

### Correctness

- [ ] Every case has an immutable initial image, SQL, bindings, work count,
      final row count, changed-row count, digest, and schema expectation.
- [ ] Modern outputs open in pinned SQLite and pass integrity checking.
- [ ] SQLite outputs reopen in Modern and match exact supported contents.
- [ ] Rollback restores byte-identical initial bytes.
- [ ] No sidecar survives a successful repetition.

### Configuration

- [ ] Effective engine-default settings are queried and retained.
- [ ] Matched SQLite settings are explicitly applied and read back.
- [ ] Modern configuration claims match reviewed defaults rather than
      inferred timing behavior.
- [ ] Page size, cache, journal, sync, mmap, temp, locking, and thread
      settings are recorded per raw report.

### Timing and statistics

- [ ] Every repetition starts from a fresh copy and connection.
- [ ] Copy, open, configure, prepare, close, and verify remain outside the
      measured interval.
- [ ] Transaction terminal I/O remains inside the measured interval.
- [ ] Every repetition lasts at least 20 ms.
- [ ] Three rounds, three repetitions, engine/profile/case order, and medians
      are predeclared.
- [ ] No sample is discarded.

### Diagnostics

- [ ] Timing binaries contain no instrumentation or allocation override.
- [ ] Counting VFS delegates every operation without semantic change.
- [ ] Modern and SQLite counter namespaces remain distinct.
- [ ] Diagnostic work and final state match timing-work definitions.
- [ ] Allocation and I/O counts are never inferred from elapsed time.

### Artifacts and CI

- [ ] All fixtures, SQL, manifests, tools, binaries, reports, and logs are
      hashed.
- [ ] Output paths cannot alias inputs or existing evidence.
- [ ] Failure preserves raw output and scratch images.
- [ ] CI validates committed evidence and smoke contracts without fresh
      throughput gating.
- [ ] Matched guard and engine-default informational status cannot be
      confused.

## Rejected Alternatives

### Time writes in the read benchmark process

Rejected because mutating repetitions require fresh images, profile-specific
configuration, final integrity checks, and different completion schemas.

### Compare only `synchronous=OFF`

Rejected because it removes the durability work the writable MVP is designed
to provide and would produce an irrelevant favorable number.

### Apply a guard to unmatched defaults

Rejected because a ratio is not a fair engine comparison when durability or
cache policy differs. Default results remain useful application evidence.

### Reuse one database across repetitions

Rejected because later repetitions would execute different B-tree,
freelist, schema, and file-growth work.

### Include fixture copying or verification in timed regions

Rejected because those operations are harness work, not engine write
execution.

### Add benchmark-only production tuning APIs

Rejected because cache and durability APIs require product requirements, not
measurement convenience.

### Optimize before recording the baseline

Rejected because the node's purpose is to establish repeatable evidence and
identify bottlenecks before mechanism changes.

## Consequences

### Positive

- Write performance becomes attributable to exact public SQL work,
  durability, I/O, allocation, and final-state contracts.
- Default product behavior and fair matched comparison are both retained
  without conflation.
- Every repetition starts from identical work and ends with bidirectional
  correctness proof.
- Lower-layer work counters become connected to a matched SQL-level baseline.
- Future optimization proposals can name one repeatable bottleneck and rerun
  crash/interoperability checks.

### Negative

- Full baseline generation performs many durable writes and may take several
  minutes.
- The populated fixture and raw artifacts add significant repository size.
- The write tool has separate stateful report validation in addition to the
  read tool.
- Engine-default results cannot support a fair cross-engine guard.

### Deferred

- secondary-index write maintenance;
- concurrent writers and busy handling;
- WAL, checkpoint, and snapshot performance;
- bulk load APIs;
- asynchronous or relaxed durability modes;
- triggers, foreign keys, and advanced SQL;
- attached databases and super-journals; and
- optimization of any measured bottleneck.

## References

- ADR-0008: SQLite Performance Parity Strategy
- ADR-0012: Optional Instrumentation
- ADR-0039: Pinned Read Performance Baseline
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0048: Writable SQL DML and CREATE TABLE Execution
- ADR-0049: Writable MVP Verification Harness
- SQLite `test/speedtest1.c`
