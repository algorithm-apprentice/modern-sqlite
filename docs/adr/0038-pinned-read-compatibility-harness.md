# ADR-0038: Pinned Read Compatibility Harness

- Status: Accepted
- Date: 2026-10-05

## Context

ADR-0037 exposes the first complete read-only application boundary:
`ReadSession` opens a SQLite database, prepares one supported statement, binds
typed values, steps typed rows, resets execution, and finalizes resources.
The next dependency-graph node must prove the read milestone against the
pinned SQLite 3.54.0 implementation:

> Golden databases, differential SQL traces, model tests, fuzz targets, and
> offline diagnostics.

The acceptance criterion is:

> SQLite-write/Modern-read compatibility runs reproducibly with pinned
> provenance.

Existing unit and format tests already prove individual parser, catalog,
planner, bytecode, VM, pager, B-tree, and session contracts. They do not
provide one versioned end-to-end corpus that:

- records the exact pinned SQLite result for every supported trace;
- preserves SQL value storage class and bytes instead of rendered text;
- distinguishes an intentional unsupported boundary from a compatibility
  mismatch;
- can run in CI without a system SQLite installation or network access;
- reports one reproducible failing case for offline diagnosis;
- compares generated read behavior with an independent state model; and
- exposes bounded entry points for mutation fuzzing.

Pinned SQLite's own testing tools provide useful precedents without defining
Modern SQLite's public format:

- `test/fuzzcheck.c` stores database images and SQL scripts as independently
  replayable corpus entries;
- `test/dbfuzz2.c` executes fixed read statements against mutated database
  images and interrupts excessive work;
- `tool/fuzzershell.c` treats each SQL input as a separately reproducible test
  case and applies engine limits;
- `test/ossfuzz.c` limits statement size, output work, memory, and elapsed
  execution; and
- `test/stmtrand.test` uses explicit seeds for deterministic randomized
  testing.

SQLite's C result APIs also establish the oracle extraction boundary:

- `sqlite3_column_type()` selects the original storage class;
- `sqlite3_column_int64()` and `sqlite3_column_double()` extract numeric
  values;
- `sqlite3_column_text()` followed by `sqlite3_column_bytes()` extracts exact
  UTF-8 text bytes;
- `sqlite3_column_blob()` followed by `sqlite3_column_bytes()` extracts exact
  blob bytes;
- `sqlite3_column_name()` and `sqlite3_column_decltype()` expose result
  metadata;
- `sqlite3_bind_*()` applies typed host parameters; and
- `sqlite3_prepare_v3()` publishes the exact unconsumed SQL tail.

Text rendering is not an adequate differential representation. It loses the
difference between NULL, TEXT, and BLOB; may round REAL values; cannot safely
represent embedded NUL bytes; and may conflate an INTEGER with a numerically
equal REAL.

The supported read surface remains the one accepted by ADR-0028 through
ADR-0037:

- zero or one table source;
- ordinary rowid tables, WITHOUT ROWID tables, and `sqlite_schema`;
- projection, optional `WHERE`, and optional `LIMIT`/`OFFSET`;
- literals, parameters, source columns, hidden rowid names, aliases, unary
  operators, supported binary operators, comparisons, truth tests, and
  `COLLATE`;
- the registered core scalar functions and the accepted lazy or likelihood
  forms; and
- the public prepare, bind, step, reset, and finalize lifecycle.

Joins, ordering, grouping, aggregation, DISTINCT SELECTs, compounds,
subqueries, windows, pattern operators, writes, explicit transactions,
attached databases, user-defined functions, and WAL remain outside this
node. A trace containing one of those constructs must be labeled as an
unsupported boundary. It must not be silently skipped or reported as a
pinned-SQLite compatibility failure.

This node is verification infrastructure, not the matched performance
baseline. Timing, throughput, allocation thresholds, profiling, and
performance regression gates belong to the following
`build-read-performance-baseline` node.

## Decision

Add a versioned, offline read-compatibility harness under the test and tool
trees. The production `modern_sqlite` library gains no dependency on SQLite,
Python, JSON, fuzzing runtimes, or verification-only types.

The harness has five cooperating parts:

1. pinned SQLite database fixtures with exact hashes and generation sources;
2. one machine-readable differential corpus and one committed pinned oracle;
3. a Modern-only trace runner that uses the public session API;
4. deterministic model and fuzz tests; and
5. one Python standard-library tool for oracle regeneration, verification,
   and mismatch diagnosis.

### Planned layout

```text
tests/
  compatibility/
    read-corpus-v1.json
    sqlite-oracle-profile-v1.json
    read-oracle-v1.json
  differential/
    read_compatibility_test.py
  fixtures/
    read_compatibility/
      README.md
      read-compatibility.sql
      sqlite-3.54.0-read-compatibility.db
  fuzz/
    corpus/
      read_database/
      read_sql/
    database_image_fuzz.cpp
    read_sql_fuzz.cpp
  model/
    read_session_model_test.cpp
  project/
    test_verification_layering.py
tools/
  modern_sqlite_read_trace.cpp
  read_compatibility.py
```

The exact filenames are part of the accepted implementation plan. A future
format revision adds `v2` artifacts rather than changing the meaning of a
committed `v1` artifact.

## Dependency and trust boundaries

### Modern trace runner

`modern_sqlite_read_trace` links only `modern_sqlite::modern_sqlite` and uses
only public headers, principally:

- `modern_sqlite/session/read_session.hpp`;
- `modern_sqlite/runtime/sql_value.hpp`;
- `modern_sqlite/base/result.hpp`; and
- `modern_sqlite/text/text.hpp`.

It does not include session internals, parser, binder, planner, lowering, VM,
pager, B-tree, or catalog implementation headers. The runner therefore tests
the same public boundary available to an application.

The runner accepts one versioned operation transcript on standard input:

```text
modern_sqlite_read_trace DATABASE
```

The wire format is deliberately narrower than JSON so the C++ runner does not
gain a general parser dependency:

```text
MSRT1
SQL 53454c454354203f31
BIND 1 integer -73
STEP
RESET
BIND 1 text 74657874
STEP
STEP
FINALIZE
```

SQL, TEXT, and BLOB payloads are hexadecimal bytes. REAL payloads are exactly
sixteen lowercase or uppercase hexadecimal digits containing one IEEE-754
binary64 bit pattern. INTEGER payloads are canonical signed decimal.
Bindings are one-based.

The transcript may contain ordered `BIND`, `STEP`, `RESET`, and `FINALIZE`
operations. Repeated binds to the same index are valid and observable.
Syntactically valid indices from zero through `INT_MAX` are passed to the
engine, so zero and indices above the statement's parameter count remain
observable engine outcomes. Larger values are protocol errors because the
SQLite C binding ABI accepts an `int`; allowing a wider wire value would make
the oracle truncate it.

Malformed protocol lines or payloads, an unknown operation, unreadable input,
or a transcript exceeding static harness limits are harness usage errors
written to standard error with exit code 1.

An engine open, prepare, bind, step, or finalize error is a trace outcome, not
a runner crash. The runner records one observation for every requested
operation, including operations after DONE or ERROR, reset results, repeated
bindings, and finalize results. Preparation metadata and previously emitted
rows remain present when a later operation fails.

Every statement transcript must contain exactly one explicit `FINALIZE`
operation as its final operation. SQLite double-finalize is undefined, so the
differential corpus never issues another operation after finalization. Modern
SQLite's safe idempotent-finalize extension remains covered by its focused
session tests rather than by the SQLite differential oracle. The runner still
uses RAII on malformed or failed input, but destructor-only cleanup is not
treated as an observable engine operation.

Cases expected to fail during preparation also carry a terminal `FINALIZE`
fallback. The operation is unreachable while the expected error remains, but
it makes later statement acceptance a comparable engine outcome instead of a
malformed, empty transcript.

The runner writes one canonical JSON object to standard output and exits zero
so the verifier can compare that outcome with the oracle. A deterministic row
or output limit produces a canonical `limit` outcome and still exits zero.
Failure to allocate, parse the wire protocol, serialize, or write the outcome
returns exit code 1.

### Pinned SQLite oracle

`tools/read_compatibility.py regenerate` loads an explicitly supplied SQLite
shared library through Python `ctypes`. It validates the complete
`sqlite-oracle-profile-v1.json`, not only the version string.

The profile pins:

- the SHA-256 digests of the generated `sqlite3.c` and `sqlite3.h`;
- the exact SQLite version and source ID;
- the canonical compile command and semantic preprocessor definitions;
- required and forbidden normalized `sqlite3_compileoption_get()` entries;
- relevant runtime `sqlite3_limit()` values;
- every explicitly selected `sqlite3_db_config()` behavior;
- the SHA-256 of `tools/read_compatibility.py`;
- the SHA-256 of each fixture-generation SQL file; and
- the profile schema version.

The canonical shared library is built from the pinned amalgamation with:

```text
-DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=3
```

and the platform's ordinary position-independent shared-library flags.
`SQLITE_OMIT_DECLTYPE`, `SQLITE_OMIT_FLOATING_POINT`, and other profile
forbidden options are rejected. The profile pins the exact sorted semantic
compile-option set, so any unlisted option is also rejected.
Compiler-identification compile options are recorded diagnostically but
excluded from normalized cross-platform equality and from the committed
oracle.

It rejects the library unless both identity values match:

```text
sqlite3_libversion(): 3.54.0
sqlite3_sourceid():
  2026-10-02 20:18:07
  65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2
```

The source ID comparison uses the complete single-line value, including the
timestamp and Fossil check-in. For every opened connection the generator
sets and verifies:

- `SQLITE_DBCONFIG_DQS_DML = 1`;
- `SQLITE_DBCONFIG_DQS_DDL = 1`;
- `SQLITE_DBCONFIG_REVERSE_SCANORDER = 0`;
- `SQLITE_DBCONFIG_ENABLE_COMMENTS = 1`; and
- `SQLITE_DBCONFIG_FP_DIGITS = 17`.

It also disables extended result-code returns with
`sqlite3_extended_result_codes(db, 0)`. Each SQLite API return value is
captured immediately, while `sqlite3_extended_errcode()` and
`sqlite3_errmsg()` are retained as diagnostics before any later API call.

The profile also fixes the runtime limits used by the harness, including
`SQLITE_LIMIT_LENGTH`, `SQLITE_LIMIT_SQL_LENGTH`, `SQLITE_LIMIT_COLUMN`,
`SQLITE_LIMIT_EXPR_DEPTH`, `SQLITE_LIMIT_VDBE_OP`,
`SQLITE_LIMIT_FUNCTION_ARG`, and `SQLITE_LIMIT_VARIABLE_NUMBER`. The
generator sets each value, reads it back, and records the effective result
instead of relying on process defaults.

The script does not load the host Python `sqlite3` module as the oracle
because that module may use an unrelated system SQLite version or compile
profile.

The pinned library is regeneration input only. It is not committed, linked
into Modern SQLite, downloaded by tests, or required by ordinary CI.

The regeneration command uses the SQLite C API directly to:

1. open each fixture read-only;
2. apply and verify the canonical connection profile;
3. prepare the exact SQL byte sequence;
4. record the tail byte offset, parameter count, and parameter names;
5. execute the ordered bind, step, reset, and finalize transcript;
6. record result-column names and declared types;
7. encode every row value by original storage class; and
8. record every raw result code immediately after its operation.

The generator finalizes every statement and closes every connection on every
path. An implicit cleanup result never overwrites the return code already
captured for an explicit operation.

The `ctypes` ABI is part of the profile implementation contract:

- SQL buffers, prepare tails, column TEXT pointers, and column BLOB pointers
  use `c_void_p`, not `c_char_p`;
- a `create_string_buffer` containing the exact SQL bytes plus one trailing
  NUL remains alive through preparation;
- the tail offset is the checked integer difference between the returned
  pointer and `addressof()` that original buffer;
- result extraction calls `sqlite3_column_type()`, then the matching value
  accessor, then `sqlite3_column_bytes()` for TEXT or BLOB, and immediately
  copies `ctypes.string_at(pointer, length)`;
- a null pointer with zero BLOB length is a valid empty BLOB;
- a failed conversion allocation is detected immediately through the
  connection error code and aborts regeneration rather than becoming NULL;
- REAL values cross the ABI as `c_double` and are packed into an
  endian-independent 64-bit bit string; and
- TEXT bindings are validated as well-formed UTF-8, retain explicit byte
  lengths including embedded NULs, and use `SQLITE_TRANSIENT`.

### Committed oracle

Ordinary verification compares the Modern runner with
`read-oracle-v1.json`. This makes the compatibility suite:

- independent of a system SQLite installation;
- independent of network access;
- reproducible on macOS and Linux;
- inspectable in code review; and
- stable if a developer upgrades unrelated host software.

Regeneration is an explicit reviewed operation. A corpus, fixture, or pinned
SQLite change must regenerate the oracle and update provenance together.

## Differential corpus

`read-corpus-v1.json` is UTF-8 JSON with a top-level `schema_version` of 1.
It contains:

- the exact oracle-profile identifier;
- a list of logical database IDs, repository-relative paths, and SHA-256
  digests;
- deterministic harness limits;
- ordered trace cases; and
- an explicit supported-matrix summary.

Every case has:

- a unique stable ASCII `id`;
- one database ID;
- exact SQL bytes as lowercase hexadecimal;
- an ordered operation transcript;
- an `expectation` of `compare` or `unsupported`; and
- a short feature category.

No case is implicitly disabled. Unknown expectation values, duplicate case
IDs, missing databases, stale database hashes, malformed operations, or
values outside the harness limits invalidate the corpus before any engine is
run. Repeated binding operations are valid. Duplicate JSON object keys are
rejected during parsing rather than silently keeping one value.

### Required comparison cases

`expectation: "compare"` means the SQL is in the current supported matrix.
The committed oracle must contain exactly one outcome for the case, and the
Modern result is compared with that outcome after removing the explicitly
diagnostic return-code, extended-code, and message fields.

The initial corpus covers at least:

- empty and trivia-only input;
- first-statement slicing and tail offsets;
- parameter count, holes, repeated names, and exact parameter spelling;
- NULL, signed INTEGER boundaries, REAL bit patterns, TEXT bytes, BLOB bytes,
  and embedded NUL text read from a database;
- typed NULL, INTEGER, REAL, TEXT, and BLOB bindings;
- result-column names and declared types;
- arithmetic, bitwise, concatenation, comparison, truth, and collation
  behavior;
- `typeof`, `length`, `abs`, `lower`, `upper`, `sign`, `nullif`, scalar
  `min`/`max`, `coalesce`, `ifnull`, `iif`/`if`, `likely`, `unlikely`, and
  `likelihood`;
- constant rows, compile-time empty results, table scans, rowid lookups,
  aliases, and wildcard expansion;
- ordinary rowid, WITHOUT ROWID, `sqlite_schema`, physically short records,
  default substitution, and overflow payloads;
- positive, zero, negative, and invalid LIMIT/OFFSET values; and
- supported parse, bind, and execution errors such as missing tables,
  missing columns, invalid function arity, and integer overflow.

The operation transcripts additionally cover:

- rebinding one parameter before execution;
- out-of-range binding as an engine result;
- reset after ROW, DONE, and ERROR;
- automatic execution restart after DONE and ERROR;
- bindings surviving reset;
- direct finalize after an execution error;
- reset followed by finalize after an execution error; and
- finalize after a successful reset.

Queries that may return multiple rows use database layouts and predicates for
which the current SQLite and Modern access paths have the same deterministic
B-tree order. The corpus does not claim SQL-standard ordering where no order
is defined, and it does not use an ordinary SQLite index to manufacture an
order that the current Modern optimizer cannot select.

### Unsupported boundary cases

`expectation: "unsupported"` means pinned SQLite accepts or recognizes a
feature intentionally outside the current Modern milestone. Such a case:

- names the excluded feature;
- has a pinned boundary record proving SQLite's prepare result and, when
  relevant, first-step result;
- records the expected Modern error phase and primary code; and
- includes a terminal `FINALIZE` fallback that is ignored while preparation
  fails but produces a complete trace if Modern later accepts the statement;
  and
- is reported as an unsupported-boundary regression if Modern silently
  accepts it or rejects it differently.

The public read-session error currently exposes a primary code and message,
not the lower parser or binder diagnostic enum. V1 therefore claims only a
coarse, authenticated boundary: pinned SQLite accepts or recognizes the
statement while Modern rejects it at the recorded phase and primary code. It
does not claim that the harness can prove which internal unsupported-feature
branch produced that rejection. Full English error messages are retained in
mismatch diagnostics but are not equality fields.

Pinned boundary records retain only recognition state such as prepare
success, first-step ROW/DONE, or first-step error code. They do not retain
dynamic row values, which keeps current-time and similar recognition probes
deterministic.

The initial boundary set covers at least:

- `SELECT DISTINCT`;
- aggregate or wildcard function calls;
- pattern operators;
- joins;
- `ORDER BY`;
- subqueries;
- writes;
- explicit transactions; and
- current-time literals.

Unsupported boundary failures are never labeled SQLite compatibility
mismatches. When a later dependency-graph node implements a feature, that
node moves its cases from `unsupported` to `compare` and regenerates the
pinned oracle.

## Canonical outcome format

Every engine outcome has `format_version: 1` and one `kind`. Every explicit
engine operation records its primary result before any cleanup call can
overwrite it. Diagnostic return, extended-code, and message fields retain
engine details when available; equality uses the normalized primary code.

Status-bearing operations use:

```json
{
  "primary_code": "ok",
  "return_code": 0,
  "extended_code": 0
}
```

or:

```json
{
  "primary_code": "type_mismatch",
  "return_code": 20,
  "extended_code": 20,
  "message_hex": "6461746174797065206d69736d61746368"
}
```

The return code, extended code, and message bytes are diagnostic. The
verifier compares `primary_code` and the operation boundary, while preserving
all diagnostic values in a mismatch report.

### Open or prepare error

```json
{
  "format_version": 1,
  "kind": "open_error",
  "status": {
    "primary_code": "cannot_open",
    "return_code": 14,
    "extended_code": 14
  }
}
```

```json
{
  "format_version": 1,
  "kind": "prepare_error",
  "status": {
    "primary_code": "generic",
    "return_code": 1,
    "extended_code": 1
  }
}
```

No tail offset is claimed after prepare failure because the public Modern
prepare error does not publish one.

### Empty statement

```json
{
  "format_version": 1,
  "kind": "empty",
  "next_offset": 8
}
```

### Statement transcript

```json
{
  "format_version": 1,
  "kind": "statement",
  "preparation": {
    "next_offset": 17,
    "parameter_count": 1,
    "parameter_names": ["3f31"],
    "columns": [
      {
        "name_hex": "6964",
        "declared_type_hex": "494e5445474552"
      }
    ]
  },
  "observations": [
    {
      "operation_index": 0,
      "op": "bind",
      "index": 1,
      "status": {
        "primary_code": "ok",
        "return_code": 0,
        "extended_code": 0
      }
    },
    {
      "operation_index": 1,
      "op": "step",
      "result": "row",
      "return_code": 100,
      "columns": [
        {
          "name_hex": "6964",
          "declared_type_hex": "494e5445474552"
        }
      ],
      "row": [
        {
          "type": "integer",
          "value": "1"
        }
      ]
    },
    {
      "operation_index": 2,
      "op": "step",
      "result": "done",
      "return_code": 101
    },
    {
      "operation_index": 3,
      "op": "finalize",
      "status": {
        "primary_code": "ok",
        "return_code": 0,
        "extended_code": 0
      }
    }
  ]
}
```

Names and declared types use hexadecimal UTF-8 bytes so the C++ runner does
not need a second arbitrary-string escaping policy. A missing parameter name
or declared type is JSON `null`. Every ROW observation republishes current
result metadata so an automatic reprepare cannot change metadata without
becoming observable.

SQL values use exactly one representation:

| Storage class | Representation |
|---|---|
| NULL | `{"type":"null"}` |
| INTEGER | `{"type":"integer","value":"<signed decimal>"}` |
| REAL | `{"type":"real","bits":"<16 lowercase hex digits>"}` |
| TEXT | `{"type":"text","hex":"<lowercase bytes>"}` |
| BLOB | `{"type":"blob","hex":"<lowercase bytes>"}` |

INTEGER uses a string to avoid JSON number precision loss. REAL uses the exact
binary64 bits so `-0.0`, infinities, and distinct finite representations
remain observable. The initial bound-value corpus is finite; the result
format still remains valid for every binary64 output.

STEP observations use one of:

```json
{
  "op": "step",
  "result": "error",
  "status": {
    "primary_code": "type_mismatch",
    "return_code": 20,
    "extended_code": 20
  }
}
```

or `row` and `done` as shown above. `BIND`, `RESET`, and `FINALIZE` always
record a status. This preserves the distinct step, reset, and finalize return
values after an execution error.

Modern SQLite's primary codes intentionally equal SQLite primary result
codes, so the oracle maps the low eight bits of every error result through the
same names. SQLite's `SQLITE_ROW` and `SQLITE_DONE` remain step result states,
not error codes. The verifier normalizes away `return_code`,
`extended_code`, and `message_hex` before equality; those fields remain in
diagnostics.

Error messages are emitted in human diagnostics but are not part of the v1
equality contract. SQLite and Modern SQLite may provide different additional
wording while preserving the same primary error and lifecycle boundary.
Changing a primary code or the phase at which the supported operation fails
is a compatibility mismatch.

### Harness limit

```json
{
  "format_version": 1,
  "kind": "limit",
  "resource": "result_rows",
  "completed_observations": []
}
```

A deterministic limit reached while executing a valid transcript is
machine-readable and exits zero. If the committed pinned outcome is within
the same limit, a Modern limit outcome is a compatibility mismatch. Corpus,
oracle, allocation, protocol, serialization, or operating-system failures
remain nonzero harness-integrity failures.

## Provenance and fixture integrity

The compatibility fixture is generated from `read-compatibility.sql` by a
pinned SQLite 3.54.0 shell built from the same amalgamation and semantic
profile as the oracle library. It uses rollback-journal DELETE mode, a fixed
page size, no WAL, and passes `PRAGMA integrity_check`.

It contains:

- a rowid table with every supported storage class and collation;
- deterministic rows for the model test;
- a WITHOUT ROWID table;
- embedded NUL text and binary payloads;
- values at signed and floating boundaries; and
- payloads that require overflow-page reads.

The corpus may also reference existing pinned fixtures when they are the
authoritative evidence for a behavior, including the physically short
records from `tests/fixtures/lowering`.

`read-oracle-v1.json` records:

- `schema_version`;
- the SHA-256 and schema version of `sqlite-oracle-profile-v1.json`;
- the exact SQLite version, source ID, and normalized semantic compile
  profile;
- the SHA-256 of the pinned `sqlite3.c` and `sqlite3.h`;
- the SHA-256 of `tools/read_compatibility.py`;
- the SHA-256 of the raw corpus file;
- every referenced database path and SHA-256;
- every fixture-generation SQL path and SHA-256;
- the generator format version; and
- exactly the ordered outcomes for `compare` cases plus authenticated pinned
  boundary records for `unsupported` cases.

Verification recomputes all hashes before running a case. A stale fixture or
corpus is a harness-integrity error, not a compatibility mismatch.

The fixture README contains the reproducible shell and shared-library build
commands, integrity-check command, source and fixture hashes, normalized
compile-profile command, and oracle-regeneration command. No absolute
workstation path is committed.

## Deterministic model test

Add a C++ model test that links the public Modern SQLite library but does not
use SQLite as an oracle.

The test opens the pinned compatibility fixture and uses a fixed documented
64-bit seed to generate a bounded sequence of:

- full scans with integer predicates;
- rowid lookups;
- typed parameter rebinding;
- LIMIT and OFFSET values;
- reset and repeated execution; and
- integer, NULL, and text projections.

An in-memory C++ vector model computes the expected rows independently. The
model deliberately uses a small semantics subset whose expected behavior is
obvious and does not duplicate the SQL implementation. Every assertion
reports the seed, generated case index, template, and bindings.

The seed, iteration count, generator version, and selection algorithm are
constants in the test. The generator is SplitMix64 with the standard fixed
unsigned 64-bit transition and output mixing operations. Bounded selections
use an explicitly documented unsigned modulo operation; no
`std::uniform_int_distribution`, standard shuffle, implementation-defined
random engine, floating distribution, or ambient randomness is used.

Unsigned overflow is intentional modulo-2^64 arithmetic. A failing assertion
reports the generator version, seed, raw generated words, case index,
template, and bindings. macOS and Linux therefore execute the same generated
sequence.

## Fuzz targets

Add two optional Clang libFuzzer targets behind:

```text
MODERN_SQLITE_BUILD_FUZZERS=OFF
```

The option is off for ordinary builds. Enabling it performs a configure-time
compile-and-link probe for both `-fsanitize=fuzzer-no-link` and
`-fsanitize=fuzzer`; checking the compiler family alone is insufficient.

The fuzz build compiles a dedicated `modern_sqlite_fuzz_engine` from the same
engine source list with `-fsanitize=fuzzer-no-link`. Fuzz executables link
that instrumented library and use `-fsanitize=fuzzer`. The ordinary
`modern_sqlite` target and ordinary test executables retain their normal
compile and link flags.

The fuzz preset also enables ASan and UBSan. Leak detection is claimed only
for an ASan/LSan-capable campaign. A missing fuzzer runtime fails
configuration with an explicit diagnostic rather than producing an
uninstrumented target.

### SQL-input target

`modern_sqlite_read_sql_fuzz`:

- opens the committed pinned compatibility database;
- rejects inputs larger than 64 KiB;
- feeds every remaining byte sequence to `ReadSession::Prepare()`, including
  malformed UTF-8, high-bit identifier bytes, overlong encodings, embedded
  NULs, and invalid continuation bytes;
- applies no external bindings;
- steps at most 256 rows; and
- finalizes all created statements.

Parse, bind, unsupported-feature, and execution errors are valid outcomes.
The target looks for crashes, sanitizer findings, assertion failures, leaks,
and hangs.

### Database-image target

`modern_sqlite_read_database_fuzz`:

- rejects inputs larger than 1 MiB;
- creates one private mode-0700 process directory atomically with `mkdtemp`;
- creates each database through `openat()` with
  `O_CREAT | O_EXCL | O_NOFOLLOW` and mode 0600;
- writes the exact bytes, closes the writer, and opens the path through the
  public read session;
- unlinks the database immediately after the session has acquired its
  read-only file handle;
- prepares fixed bounded schema-table and constant-row statements;
- steps at most 256 rows per statement;
- closes all handles on every normal or error return; and
- removes the private directory on normal process exit.

Open, corruption, unsupported-format, prepare, and step errors are valid
outcomes.

RAII covers every non-crashing path. A process abort between creation and
unlink may leave only a private mode-0700 directory and, during the narrow
pre-open window, one mode-0600 file. The design does not make an impossible
no-artifact guarantee after a sanitizer abort or process crash.

Both targets expose shared bounded entry functions. Normal CI links those
functions into non-fuzzer smoke drivers and replays committed seed corpora
without the libFuzzer runtime. Fuzz campaigns link the same functions into
libFuzzer entry points and use explicit `-runs`, `-timeout`, and
`-rss_limit_mb` bounds.

No wall-clock sleep, network access, unbounded recursive SQL, unbounded row
collection, or name-based process termination is permitted.

## Offline verification and diagnostics

`tools/read_compatibility.py` uses only the Python standard library and has
two subcommands:

```text
read_compatibility.py regenerate ...
read_compatibility.py verify ...
```

`verify` accepts the repository root, corpus, oracle, Modern runner, and an
optional case ID. It validates provenance, invokes only the requested cases,
strictly validates every runner outcome, and returns:

- 0 when all selected cases match;
- 1 for invalid corpus, stale provenance, malformed runner output, usage, or
  other harness-integrity failure; and
- 2 for a compatibility or unsupported-boundary mismatch.

On mismatch it prints:

- the case ID and category;
- the database path and verified hash;
- SQL bytes;
- the ordered operation transcript;
- expected and actual canonical outcomes;
- the first structural difference; and
- a shell-quoted command and protocol input that rerun only that Modern
  trace.

Each runner subprocess has a fixed per-case timeout. Standard output and
standard error are read incrementally with hard byte ceilings; the verifier
terminates the exact child process when either ceiling or the timeout is
reached. It always checks the exit status and terminating signal, even when
standard output contains valid JSON. A nonzero exit, signal, or sanitizer
diagnostic is a harness execution failure and cannot be hidden by an earlier
JSON line. Captured stderr in reports is length-bounded.

It never rewrites the committed oracle during verification. Regeneration
writes deterministic UTF-8 JSON with stable key ordering and a trailing
newline. Its output path may not alias the corpus, profile, generator,
fixture sources, fixture databases, pinned SQLite library, amalgamation, or
generated header.

The CTest differential test invokes `verify` against the committed oracle.
It also uses a temporary deliberately altered oracle to prove that a mismatch
returns exit code 2 and includes a focused reproduction command.

## Limits

The v1 corpus and both engines use these harness limits:

| Resource | Limit |
|---|---:|
| Corpus file bytes | 4,194,304 |
| Oracle file bytes | 67,108,864 |
| Cases per corpus | 512 |
| SQL bytes per case | 65,536 |
| Operations per case | 1,024 |
| Binding operations per case | 256 |
| Result columns | 256 |
| Result rows | 4,096 |
| Bytes in one TEXT or BLOB value | 1,048,576 |
| Total canonical output per case | 16,777,216 |
| Captured child stderr | 1,048,576 |
| Child runtime per case | 10 seconds |

Corpus validation rejects a case that statically exceeds a limit. A runner
that exceeds a dynamic row or output limit emits the canonical `limit`
outcome. If the pinned case remains within the same limit, the verifier
reports exit code 2 because the Modern result diverged. Allocation,
serialization, operating-system, malformed-file, timeout, excess-capture,
and abnormal-child failures use exit code 1. The committed corpus stays far
below these ceilings.

## TDD sequence

Implementation uses focused red-green slices:

1. add the runner protocol and CLI tests, observe the missing runner behavior,
   and implement transcript parsing plus canonical lifecycle observations;
2. add Python corpus/profile validator tests using synthetic temporary JSON,
   observe failure, and implement strict bounded validation;
3. add `ctypes` extraction tests against a temporary pinned database,
   including embedded NUL text, empty BLOB, tail offsets, REAL bits, and
   step/reset/finalize error precedence, then implement regeneration;
4. add verifier and mismatch-diagnostic tests with synthetic runner/oracle
   data, observe failure, and implement bounded subprocess comparison;
5. add the committed fixture, corpus, and oracle integration test, observe the
   missing artifacts or mismatch, then generate and verify them;
6. add the fixed-generator model test, observe the first model/engine
   divergence or missing target, and implement only its independent model
   driver;
7. add shared fuzz-entry smoke tests, observe the missing bounded entry
   functions, implement them, and only then add the optional libFuzzer
   wrappers and configure probe; and
8. run the complete project validation matrix.

Each slice records its own intended failure before its implementation.
Synthetic fixtures precede committed-oracle generation so an absent golden
file is never used as the only TDD red signal.

Tests cover malformed protocol and value encodings, repeated and out-of-range
bindings, stale hashes, malformed oracle/profile data, duplicate JSON keys,
unknown cases, wrong pinned SQLite identity or semantic profile, runner
engine errors, reset/finalize precedence, unsupported boundaries, mismatch
diagnostics, timeout and output caps, nonzero or signaled children, closed
standard output, race-safe temporary-file cleanup, malformed UTF-8 fuzz
seeds, and deterministic model replay.

## Build and validation

The ordinary build adds:

- `modern_sqlite_read_trace`;
- one differential CTest labeled `differential`;
- one model executable labeled `model`;
- CLI and fuzz-corpus smoke tests; and
- verification-layering project tests.

The optional fuzz build is separate and does not change ordinary compile or
link flags. It reuses one reviewed engine source list to prevent source drift
between the ordinary and fuzz-instrumented static libraries.

Node validation includes:

- focused CLI, differential, model, fuzz-corpus, provenance, and layering
  tests;
- exact oracle regeneration with the pinned shared library and a clean diff;
- Debug and Release builds;
- ASan/UBSan and TSan suites;
- clang-tidy on ordinary compiled targets;
- formatting and whitespace checks;
- project graph validation;
- fixture SHA-256 and `PRAGMA integrity_check`;
- a bounded ASan/UBSan libFuzzer replay when the local configure-time runtime
  probe succeeds, or an explicit recorded toolchain limitation when it does
  not; and
- independent design and final implementation review.

There is no performance threshold in this node.

## Rejected alternatives

### Link SQLite into the ordinary test binary

Rejected because it introduces two database engines into one process, makes
symbol and allocator isolation harder, requires SQLite in every CI build, and
does not provide a reviewable committed oracle.

### Use the host `sqlite3` executable or Python module during CI

Rejected because their version and compile options are not pinned.

### Compare rendered shell output

Rejected because it loses storage classes, exact REAL bits, embedded NUL
bytes, and binary values.

### Compare exact error messages

Rejected for v1 because the project contract is primary error code and
lifecycle boundary. Additional wording is useful diagnostic data but not a
stable cross-implementation equality boundary.

### Treat every valid SQLite statement as a required case

Rejected because the current milestone intentionally implements a subset.
Unsupported cases must remain explicit and separately classified.

### Generate random differential SQL in ordinary CI

Rejected because it obscures reproduction and can accidentally cross the
supported boundary. The committed corpus is differential evidence; the fixed
model seed and fuzz targets provide generated coverage.

### Add JSON parsing to the production library

Rejected because JSON is a verification artifact, not a database-engine
dependency. Python owns corpus parsing, while the C++ runner only emits its
small canonical schema and parses the narrow versioned transcript protocol.

## Consequences

- The read milestone gains one reproducible compatibility command and one
  case-level reproduction path.
- CI remains offline and independent of host SQLite versions.
- Every expected value preserves its SQLite storage class and exact bytes.
- Unsupported SQL is visible without being mislabeled as a regression.
- The committed oracle becomes reviewed evidence and must be regenerated
  when corpus or fixture bytes change.
- Verification code exercises only the public read-session boundary.
- Model and fuzz coverage remain deterministic and bounded in ordinary CI.
- Optional fuzzing adds no runtime or dependency cost to production builds.
- The following performance-baseline node can reuse the same fixture and
  correctness corpus without changing this node's semantic evidence.
