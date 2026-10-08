# Write Performance Baseline

ADR-0050 defines the authoritative public-session write benchmark. The
baseline compares Modern SQLite with pinned SQLite 3.54.0 under separate
`engine-default` and `matched-durable` profiles.

## Build

Configure the benchmark preset with the pinned amalgamation, then build both
executables:

```sh
cmake --preset benchmark \
  -DMODERN_SQLITE_PINNED_SQLITE_C=/absolute/path/to/sqlite3.c \
  -DMODERN_SQLITE_PINNED_SQLITE_H=/absolute/path/to/sqlite3.h
cmake --build --preset benchmark --parallel 3 --target \
  modern_sqlite_write_benchmark \
  modern_sqlite_write_diagnostics
```

Confirm both binaries report the same source and SQLite identity. Only the
diagnostic binary may report instrumentation:

```sh
build/benchmark/modern_sqlite_write_benchmark identity
build/benchmark/modern_sqlite_write_diagnostics identity
```

## Generate and validate evidence

Generation requires a clean committed worktree, binaries configured from that
exact revision and tree, and an absent output directory:

```sh
python3 tools/write_performance.py generate-baseline \
  --repository-root . \
  --workloads tests/performance/write-workloads-v1.json \
  --timing-binary build/benchmark/modern_sqlite_write_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_write_diagnostics \
  --output benchmarks/write-baseline-v1
```

Exit code `2` means the complete structurally valid baseline was written but
the matched-durable 10x guard failed. Do not delete or replace such a run to
obtain a favorable sample. Exit code `1` means a harness, provenance,
correctness, process, timeout, or artifact failure.

Offline validation never reruns timing:

```sh
python3 tools/write_performance.py validate-baseline \
  --repository-root . \
  --workloads tests/performance/write-workloads-v1.json \
  --baseline benchmarks/write-baseline-v1
```

## Pre-commit baseline finding

Two independent full-generation probes produced the same schedules, work,
provenance, logical results, database fingerprints, diagnostic counter
families, and stable primary guard blocker.

`matched-durable/delete-scan-implicit` measured approximately 10.41--10.49x
SQLite wall time and 8.96--9.08x process CPU. All three paired wall rounds
exceeded the 10x guard. The diagnostic replay recorded:

- 4,380 Modern database-page writes versus 73 SQLite cache writes;
- 7,738 Modern main-journal sync calls;
- 1,168,839 Modern B-tree comparisons; and
- 917,509 Modern VM instructions versus 196,616 SQLite VM steps.

The baseline therefore records a validated guard failure instead of widening
the threshold or replacing samples. ADR-0008's optimization-admission process
must address this case in a separate reviewed node.

## Canonical baseline result

The committed `benchmarks/write-baseline-v1` run records:

- `delete-scan-implicit`: 10.44x aggregate wall and 9.02x CPU, with all
  paired wall rounds between 10.24x and 10.87x;
- `update-scan-implicit`: 9.54x aggregate wall and 8.64x CPU, with round 0
  wall at 10.07x; and
- every other matched aggregate and paired-round wall/CPU ratio below 10x.

`engine-default` remains informational. ADR-0051 admits a separate
SQLite-aligned one-pass scan-mutation optimization node; the canonical
baseline and guard remain unchanged until that node is remeasured.

## Fixed profile replay

Profile replay is diagnostic evidence, not baseline timing. It performs one
untimed warmup on a fresh copy and one requested fixed-work execution on a
second fresh copy:

```text
modern_sqlite_write_benchmark \
  profile ENGINE PROFILE CASE INPUT SCRATCH WORK
```

`SCRATCH` must be an existing empty directory. The command verifies both
outputs, removes owned database files and sidecars on success, and emits the
logical result and database fingerprint.

Example:

```sh
mkdir /tmp/modern-sqlite-write-profile
build/benchmark/modern_sqlite_write_benchmark \
  profile modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-profile \
  65536
```

### macOS Time Profiler

```sh
xcrun xctrace record \
  --template 'Time Profiler' \
  --output /tmp/modern-write.trace \
  --launch -- \
  build/benchmark/modern_sqlite_write_benchmark \
  profile modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-profile \
  65536
```

### Linux perf

```sh
perf stat -- \
  build/benchmark/modern_sqlite_write_benchmark \
  profile modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-profile \
  65536

perf record -g -- \
  build/benchmark/modern_sqlite_write_benchmark \
  profile modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-profile \
  65536
```

### Optional Cachegrind

When Valgrind supports the host and target:

```sh
valgrind --tool=cachegrind \
  build/benchmark/modern_sqlite_write_benchmark \
  profile modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-profile \
  65536
```

Cachegrind output is diagnostic only and cannot replace the uninstrumented
baseline samples.

### Counter replay

The diagnostic binary executes the manifest's full baseline work once after
warmup and emits Modern instrumentation, counting-VFS, or SQLite native
counters:

```sh
mkdir /tmp/modern-sqlite-write-diagnostic
build/benchmark/modern_sqlite_write_diagnostics \
  run modern matched-durable delete-scan-implicit \
  tests/fixtures/write_performance/populated.db \
  /tmp/modern-sqlite-write-diagnostic \
  diagnostic
```
