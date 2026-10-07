# ADR-0049: Writable MVP Verification Harness

- Status: Accepted
- Date: 2026-10-08

## Context

Nodes 33 through 38 provide the complete single-connection writable SQL path:

- a DELETE-mode rollback journal and writable Pager;
- reference-faithful table and index B-tree mutation;
- implicit and explicit transactions, statement rollback, and named
  savepoints;
- basic `CREATE TABLE`, `INSERT`, `UPDATE`, and `DELETE`;
- a public `WriteSession` prepare/bind/step/reset/finalize boundary; and
- pinned-SQLite differential and bidirectional smoke coverage.

The `build-writable-mvp-harness` node must now deliver:

> Bidirectional format, crash, model, sanitizer, fuzz, and integrity-check
> harness for the writable MVP.

Its acceptance criterion is:

> Modern-write/SQLite-read and SQLite-write/Modern-read matrices pass
> reproducibly.

The existing evidence is necessary but not sufficient as the milestone
closure:

- ADR-0038 provides a versioned read-only corpus, committed oracle, public
  trace runner, deterministic model, and bounded read fuzz targets.
- ADR-0042 through ADR-0044 provide exhaustive Pager and B-tree fault,
  compatibility, and crash evidence at storage boundaries.
- ADR-0047 provides coordinator state-machine, savepoint, cleanup, and crash
  evidence below SQL compilation.
- ADR-0048 provides focused SQL behavior, OOM, allocation, lifecycle, a live
  pinned-SQLite statement trace, and basic bidirectional file tests.

Three integrated gaps remain:

1. no deterministic crash matrix enters through `WriteSession` and therefore
   covers SQL compilation, VM execution, transaction coordination, catalog
   publication, and recovery as one public operation;
2. no independent writable state model checks mixed SQL, connection counters,
   transaction state, and table contents after every action; and
3. no write-capable fuzz entry point executes arbitrary SQL scripts or fixed
   mutations against arbitrary database images through the public writable
   session.

The lower-layer crash suites must remain authoritative for page topology,
freelist algorithms, journal framing, sector ordering, sibling balancing, and
every storage mutation cut. Node 39 composes those mechanisms through the
public SQL boundary; it does not duplicate their internal test matrices.

Pinned SQLite 3.54.0 supplies the verification precedents:

- `test/fuzzcheck.c:16-79` runs SQL corpora against database-image corpora and
  retains case identity for reproduction;
- `test/dbfuzz2.c:13-52` runs a fixed write-capable SQL sequence against
  mutated database images;
- `test/ossfuzz.c:116-180` uses a fresh writable database plus explicit
  statement, output, memory, and runtime limits;
- `test/crash4.test:48-97` accepts only a checksum from a known legal
  statement boundary after a crash;
- `test/crash6.test:64-112` repeats crash recovery across page-size variants;
- `test/crash8.test:57-113` combines large and small transactions around
  recovery-sensitive journal states; and
- `test/stmtrand.test:14-20` uses a deterministic seeded pseudo-random
  sequence.

Modern SQLite already has stronger deterministic fault injection than
SQLite's delay-based crash process. `WritePagerMemoryVfs` numbers every
persistent mutation and can restore only durable bytes. Node 39 reuses that
mechanism through `WriteSession` rather than adding process sleeps, signal
timing, or a second journal model.

This node is verification infrastructure. It adds no SQL feature, public
database behavior, performance claim, production dependency on SQLite, or
production fault-injection API. Matched write performance belongs to Node 40.

## Decision

### 1. Add one writable-MVP verification layer

Add bounded test-only infrastructure under the existing test tree:

```text
tests/
  compatibility/
    write_session_crash_harness.hpp
    write_session_crash_harness.cpp
    write_session_crash_test.cpp
    write_session_sqlite_compatibility.cpp
  fuzz/
    corpus/
      write_database/
      write_sql/
    write_database_image_fuzz.cpp
    write_database_image_fuzzer_main.cpp
    write_fuzz.hpp
    write_fuzz_smoke_test.cpp
    write_sql_fuzz.cpp
    write_sql_fuzzer_main.cpp
  model/
    write_session_model_test.cpp
  project/
    test_verification_layering.py
```

The existing `write_session_sqlite_compatibility.cpp` remains the pinned
entry point and is expanded rather than replaced by a second overlapping
binary.

The production `modern_sqlite` target gains no SQLite, JSON, Python, test-VFS,
model, or fuzz dependency. The optional fuzz targets continue to compile the
same reviewed engine source list as the ordinary library.

### 2. Exercise the public writable session boundary

The bidirectional matrix, model, SQL-script fuzz target, and database-image
fuzz target use only public application contracts:

- `modern_sqlite/session/write_session.hpp`;
- `modern_sqlite/runtime/sql_value.hpp`;
- `modern_sqlite/base/result.hpp`;
- `modern_sqlite/base/bytes.hpp`; and
- `modern_sqlite/text/text.hpp`.

They do not include parser, binder, planner, lowering, bytecode, VM, catalog,
Pager, B-tree, transaction, or session implementation headers.

The crash harness additionally uses the deterministic test VFS because fault
injection is its purpose. It still enters database work only through
`WriteSession`; it does not call Pager, B-tree, VM, or transaction methods.

Extend `test_verification_layering.py` so these boundaries are mechanically
checked. Production sources remain forbidden from depending on SQLite,
libFuzzer entry points, test support, or verification-only code.

### 3. Use stable compiled scenarios instead of a second write oracle

The read harness needs a committed oracle because ordinary read verification
does not build SQLite and each case is a stateless observation over immutable
fixtures.

Writable verification already has a pinned-amalgamation CI contract that:

- checks the exact SQLite source commit;
- checks the generated `sqlite3.c` and `sqlite3.h` hashes;
- compiles the canonical SQLite profile; and
- links only verification targets against that pinned engine.

Node 39 therefore keeps writable scenarios as stable, named C++ case tables
and executes both engines live. It does not create a second multi-thousand-line
JSON oracle that duplicates the reference model and final database states.

Every pinned case has a unique lowercase ASCII identifier. The compatibility
binary accepts:

```text
modern_sqlite_write_session_sqlite_compatibility
modern_sqlite_write_session_sqlite_compatibility --case CASE
modern_sqlite_write_session_sqlite_compatibility \
  --case CASE --cut CUT --durability volatile
```

`--cut` and `--durability` are valid only for crash cases. Unknown cases,
duplicate options, malformed numbers, and incompatible options are harness
usage errors. Durability accepts exactly `volatile` or `durable`.

On failure, diagnostics include:

- case identifier;
- engine handoff phase;
- SQL text or action;
- page-size variant;
- crash cut and durability mode when applicable;
- expected and actual primary code, rows, counters, schema state, and file
  state; and
- one shell-quoted command that reruns only that case.

Default execution runs every case in deterministic lexical case order.

### 4. Expand the bidirectional public-session matrix

The pinned target covers three handoff directions:

1. SQLite creates and seeds a database, Modern mutates it, and SQLite verifies
   it.
2. Modern creates and mutates a database, SQLite mutates it, and Modern
   reopens and verifies it.
3. SQLite and Modern alternate ownership across multiple committed
   transactions.

SQLite-created inputs cover page sizes:

```text
512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
```

Modern-created empty databases use the current public default page size.
Changing the public page-size configuration is outside this verification
node.

The matrix uses only Node 38's accepted table surface:

- ordinary rowid tables;
- tables with and without an `INTEGER PRIMARY KEY` alias;
- nullable and NOT NULL columns;
- constant defaults;
- INTEGER, REAL, TEXT, BLOB, and NULL storage classes;
- negative, zero, ordinary positive, and signed-boundary explicit rowids;
- local and overflow payloads; and
- no secondary or automatic index requiring index maintenance.

For every applicable handoff, exercise:

- generated and explicit rowids;
- typed parameter binding for every storage class, embedded-NUL TEXT, and
  binary BLOB values;
- exact and scan UPDATE;
- rowid-preserving and rowid-changing UPDATE;
- exact and scan DELETE;
- duplicate-rowid, NOT NULL, malformed-rowid, and fractional-rowid errors;
- `CREATE TABLE` and `IF NOT EXISTS`;
- implicit transactions;
- explicit `BEGIN`, `COMMIT`, and `ROLLBACK`;
- nested named savepoints, `ROLLBACK TO`, and `RELEASE`;
- schema creation followed by commit, full rollback, and rollback-to;
- connection `changes`, `last_insert_rowid`, and autocommit transitions; and
- reopen after each engine handoff.

Verification compares:

- primary error and finalize codes at the same operation boundary;
- result-column count, row count, storage class, and exact value bytes;
- catalog rows and stored schema SQL;
- schema cookie, page count, and freelist count;
- final logical table contents; and
- the absence of a partial user or schema mutation.

Pinned SQLite runs `PRAGMA integrity_check` after every ownership handoff and
at the final state. Modern reopens the same image and reads every supported
table after SQLite-owned writes.

Tables outside Node 38's mutation surface, including indexed and WITHOUT
ROWID tables, are boundary cases. Modern must reject their mutation before a
persistent change, and SQLite must confirm that the image remains unchanged
and valid.

### 5. Add public-session deterministic crash composition

Add a reusable crash harness that executes named SQL scenarios through
`WriteSession` over `WritePagerMemoryVfs`.

The VFS test support gains a verification-only durable-image snapshot
contract that can preserve:

- main database bytes and presence;
- rollback-journal bytes and presence;
- subjournal bytes and presence;
- reported sizes and durable sizes; and
- the crash cut metadata needed for diagnostics.

The snapshot contains no live file object, lock, borrowed view, or production
type. A new VFS instance loads the snapshot for recovery. This is necessary
because `WriteSession` intentionally owns its VFS; production ownership is
not weakened for testing.

For each scenario:

1. create one deterministic initial image;
2. run the scenario without a cut to capture its complete terminal image and
   persistent mutation count;
3. rerun from the same initial image with a cut after every numbered
   persistent mutation;
4. while the cut session still owns the VFS, call a test-only
   `CrashAndSnapshot()` operation through a retained non-owning test pointer;
5. freeze the restored durable main/journal/subjournal snapshot before
   destroying the failed session, so destructor cleanup can affect only the
   discarded live VFS;
6. load that frozen snapshot into a new VFS;
7. recover through a fresh `WriteSession`;
8. repeat recovery once to prove idempotence; and
9. classify the recovered state against the scenario's legal states.

Every case runs with database writes modeled as both volatile and immediately
durable. A scenario that exceeds 2048 persistent mutation points is rejected
as an invalid harness case rather than creating an unbounded CI loop.

The initial public-session crash set covers:

- implicit CREATE TABLE commit;
- implicit INSERT commit;
- exact rowid-moving UPDATE commit;
- rowid-changing scan UPDATE commit;
- scan DELETE commit;
- explicit transaction commit after multiple DML statements;
- a failed constraint statement preserving earlier explicit-transaction
  work before commit;
- named savepoint rollback followed by outer commit;
- transaction-savepoint release;
- CREATE TABLE rolled back by full rollback and by rollback-to;
- full transaction rollback; and
- abandoned statement or session cleanup proving that destruction never
  creates a hidden commit.

The public composition matrix does not repeat quick balance, sibling
selection, root depth, freelist trunk, overflow-chain, or page-rekey cases.
ADR-0044 remains authoritative for those internal transitions.

A recovered crash image is valid only when:

- its main database is byte-identical to one declared legal terminal image;
- hot-journal recovery is idempotent;
- Modern reads the expected catalog and rows;
- pinned SQLite reads the same state;
- `PRAGMA integrity_check` returns `ok`;
- schema cookie and freelist count match the legal state; and
- no statement, savepoint, or catalog mutation is partially visible.

The ordinary crash executable performs the full cut matrix and Modern checks
without SQLite. The pinned compatibility executable invokes the same shared
case definitions and adds SQLite verification. This keeps ASan/UBSan coverage
without duplicating scenario logic.

### 6. Add an independent writable state model

Add one deterministic `WriteSession` model test using a fixed SplitMix64
generator and standard containers. It uses the public path-based session
over one process-private temporary database so deterministic close/reopen
checkpoints exercise the ordinary POSIX ownership path.

The model owns:

- a set of visible table definitions in the accepted schema subset;
- an ordered `std::map<std::int64_t, ModelRow>` for the main table;
- an optional outer-transaction snapshot;
- an ordered vector of named savepoint snapshots;
- expected autocommit state;
- expected `changes`; and
- expected `last_insert_rowid`.

It generates typed actions, not arbitrary SQL grammar. Each action selects
one reviewed SQL template and typed operands. This keeps the model independent
from parsing and prevents generated cases from silently leaving Node 38's
supported surface.

The fixed action set covers:

- CREATE and IF NOT EXISTS;
- explicit-rowid and default INSERT;
- exact and scan UPDATE;
- rowid movement and duplicate conflicts;
- exact and scan DELETE;
- duplicate-rowid, NOT NULL, and rowid type errors;
- SELECT snapshots;
- BEGIN, COMMIT, and ROLLBACK;
- SAVEPOINT, RELEASE, and ROLLBACK TO;
- schema creation inside transaction and savepoint scopes; and
- invalid transaction commands.

Named-savepoint actions include duplicate names and ASCII-case-insensitive
lookup so the model exercises newest-match behavior rather than only unique
names.

After every action, compare:

- success or primary error;
- typed SELECT rows;
- table and catalog visibility;
- `changes`;
- `last_insert_rowid`; and
- autocommit state.

At deterministic commit and rollback checkpoints, close and reopen the
session and compare the persistent model again.

The initial model constants are:

| Property | Value |
|---|---:|
| Generator | `splitmix64-v1` |
| Seed | `0x4d53514c57524954` |
| Actions | 256 |
| Maximum rows | 128 |
| Maximum named savepoints | 8 |

The generator uses defined unsigned modulo-2^64 arithmetic and explicit
unsigned modulo selection. It does not use ambient randomness,
`std::uniform_int_distribution`, implementation-defined shuffling, or wall
clock data.

Failure output includes the generator version, seed, action index, raw random
words, selected template, operands, the bounded prior action history, and the
first model/engine difference.

### 7. Add bounded write fuzz entry points

Add two shared fuzz functions and compile them both into ordinary smoke tests
and optional libFuzzer targets.

#### SQL-script input

`RunWriteSqlInput()`:

- rejects input larger than 64 KiB;
- creates one fresh private mode-0700 directory and mode-0600 database;
- seeds one small supported table through `WriteSession`;
- treats the input as an exact SQL script;
- repeatedly calls `Prepare()` on the remaining tail;
- executes at most 32 statements;
- performs at most 4096 total step calls and 256 row results per statement;
- finalizes every prepared statement;
- explicitly rolls back an unfinished transaction when possible; and
- removes all named files and the private directory on normal return.

Prepare, unsupported-feature, constraint, transaction, corruption,
execution, rollback, and finalize errors are valid fuzz outcomes. The target
looks for crashes, sanitizer findings, assertions, leaks, hangs, stale
ownership, and invalid memory access.

#### Database-image input

`RunWriteDatabaseImageInput()`:

- rejects input larger than 1 MiB;
- writes the exact bytes to a private mode-0600 database using `openat()` with
  `O_CREAT | O_EXCL | O_NOFOLLOW`;
- opens it through `WriteSession`;
- executes one fixed bounded sequence containing schema read, conditional
  CREATE, INSERT, exact UPDATE, exact DELETE, savepoint rollback, and final
  read;
- keeps the path linked until the writable session and journal are closed;
- limits final database growth to the fixed sequence;
- treats open, format, corruption, schema, and SQL errors as valid outcomes;
  and
- cleans the database, journal, and private directory on normal return.

This follows SQLite's `fuzzcheck.c` database-times-SQL model and
`dbfuzz2.c` fixed-SQL-on-mutated-image model without introducing a general
fuzz protocol into production.

Committed smoke corpora include:

- valid CREATE/INSERT/UPDATE/DELETE scripts;
- explicit transaction and savepoint scripts;
- duplicate, NOT NULL, and malformed-rowid failures;
- empty, embedded-NUL, invalid-UTF-8, truncated, and random SQL;
- empty, truncated-header, random, read-fixture, and writable-fixture
  database images.

Rollback-journal bytes are not encoded into the database-image fuzz input.
Hot-journal coverage belongs to the deterministic crash harness, which
preserves main and journal files as one reproducible state.

The optional fuzz executables reuse `modern_sqlite_fuzz_engine`, ASan, UBSan,
and the existing configure-time libFuzzer capability probe. No fuzzer runtime
flag reaches the ordinary engine target.

### 8. Keep execution bounded and reproducible

The harness uses these limits:

| Resource | Limit |
|---|---:|
| Pinned compatibility cases | 128 |
| Crash mutation cuts per case | 2048 |
| Model actions | 256 |
| Model rows | 128 |
| Model savepoints | 8 |
| Fuzz SQL bytes | 65,536 |
| Fuzz database bytes | 1,048,576 |
| Fuzz statements per input | 32 |
| Fuzz total step calls | 4,096 |
| Fuzz rows per statement | 256 |
| One TEXT or BLOB observation | 1,048,576 |
| Compatibility test timeout | 120 seconds |
| Ordinary model/fuzz test timeout | 20 seconds |

No harness uses network access, wall-clock sleeps, unbounded result
collection, unbounded SQL recursion, ambient randomness, name-based process
termination, or a shared persistent fuzz database.

Temporary paths are process-private. Cleanup targets only exact files and the
one exact directory created by the case. A sanitizer abort may leave that
private directory; the design does not claim impossible cleanup after
process termination.

### 9. Integrate with existing CI without another full build

Ordinary Debug, Release, GCC, ASan/UBSan, and Clang-Tidy builds gain:

- the writable model test;
- the Modern-only public-session crash test; and
- the write fuzz smoke test.

The existing pinned SQLite benchmark-contract job additionally builds and
runs the expanded writable-session compatibility target. It does not create a
second configure/build job.

The sanitizer job already builds the ordinary test graph, so the shared write
fuzz entry functions and Modern-only crash matrix run under ASan/UBSan without
building a duplicate engine. Optional libFuzzer campaigns remain explicit
developer or scheduled work through the existing `fuzz` preset.

Node 39 adds no timing or throughput threshold. Node 40 may reuse its fixtures
and final-state checks but must establish separate matched performance
workloads.

### 10. Deliver the harness in red-green slices

Implementation order is:

1. add the writable model action definitions and first failing public-session
   comparison, then implement the complete deterministic model;
2. add SQL-script fuzz smoke seeds and observe the missing entry point, then
   implement bounded script execution;
3. add database-image fuzz smoke seeds and observe the missing entry point,
   then implement bounded fixed mutations and cleanup;
4. add the crash-image snapshot test seam and one failing implicit INSERT
   crash case, then implement shared public-session crash replay;
5. expand crash cases one coherent transaction/catalog family at a time;
6. add page-size and engine-handoff compatibility cases to the pinned target;
7. add case filtering, reproduction diagnostics, layering checks, and CI
   wiring; and
8. run the complete project validation and independent final review.

Each slice records its intended failure before implementation. Verification
support may be refactored only while all completed slices remain green.
Production behavior changes are out of scope; any discovered engine defect is
fixed in the owning Node 38 module with its own focused regression test before
the harness expectation is accepted.

## Build and validation

Node completion requires:

- focused model, crash, fuzz-smoke, compatibility, and layering tests;
- exact case-level reproduction for every intentional mismatch;
- bidirectional pinned-SQLite runs on macOS and Linux where the existing
  contract matrix enables them;
- `PRAGMA integrity_check` after every engine handoff and recovered state;
- Debug and Release full test tiers;
- GCC warnings-as-errors;
- ASan/UBSan;
- TSan when supported by the local toolchain;
- full Clang-Tidy, including benchmark-only translation units;
- formatting and whitespace checks;
- engineering-discipline and project-graph validation;
- a bounded optional libFuzzer corpus replay when the toolchain probe
  succeeds; and
- design and implementation review with no unresolved issue.

There is no performance baseline or optimization admission in this node.

## Rejected alternatives

### Add a second committed JSON write oracle

Rejected because the pinned write contract already executes the exact
reference engine, writable cases have stateful multi-statement images, and a
large static oracle would duplicate the independent model and legal crash
states. Stable compiled case IDs and exact pinned-source verification provide
reproducibility with less maintenance.

### Use the host SQLite executable or Python `sqlite3`

Rejected because version, compile options, default limits, and journal
behavior are not pinned.

### Generate random SQL in ordinary CI

Rejected because it obscures reproduction and can cross unsupported SQL
boundaries. The model generates typed actions, compatibility cases are fixed,
and fuzzing owns arbitrary byte input.

### Repeat every lower-layer crash topology through SQL

Rejected because ADR-0044 and ADR-0047 already exhaustively cover those
mechanisms. Node 39 proves public composition and catalog/statement
boundaries.

### Add production fault-injection or Pager configuration APIs

Rejected because verification must not weaken session ownership or expose
storage internals. The crash snapshot seam remains in test support.

### Reuse one database across fuzz inputs

Rejected because state leakage prevents deterministic reproduction and can
cause unbounded growth.

### Add a separate PR sanitizer or fuzz build

Rejected because the existing ordinary sanitizer build already compiles and
runs shared smoke entries. A duplicate engine build would increase CI time
without improving the reviewed boundary.

### Treat timing as a harness acceptance criterion

Rejected because matched write performance is the following DAG node.

## Consequences

### Positive

- The writable MVP is verified through the same public API used by an
  application.
- Every persistent SQL crash cut resolves to a declared complete state and is
  reproducible by case and cut.
- A small independent model checks connection and transaction semantics after
  every mixed action.
- Write SQL and malformed database images gain bounded sanitizer and fuzz
  coverage.
- Existing lower-layer crash evidence is composed rather than duplicated.
- CI gains the new evidence without another full configure/build matrix.

### Negative

- Public-session crash replay adds a test-only durable-image snapshot type.
- The pinned compatibility executable becomes larger and may approach its
  explicit timeout as page-size cases grow.
- The model intentionally covers only Node 38's SQL subset and must be
  extended when later nodes add indexes or advanced SQL.

### Deferred

- matched write performance and allocation baselines;
- index maintenance and index-aware write interoperability;
- multi-connection locking and busy-handler fuzzing;
- WAL crash and checkpoint matrices;
- C API lifecycle differential testing;
- triggers, foreign keys, views, virtual tables, and advanced SQL; and
- long-running randomized and distributed fuzz campaigns.

## References

- ADR-0002: SQLite Compatibility Contract
- ADR-0005: Test-Driven Development
- ADR-0006: AI-Native Sequential Workflow
- ADR-0038: Pinned Read Compatibility Harness
- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0048: Writable SQL DML and CREATE TABLE Execution
- SQLite `test/fuzzcheck.c`
- SQLite `test/dbfuzz2.c`
- SQLite `test/ossfuzz.c`
- SQLite `test/crash4.test`
- SQLite `test/crash6.test`
- SQLite `test/crash8.test`
- SQLite `test/stmtrand.test`
