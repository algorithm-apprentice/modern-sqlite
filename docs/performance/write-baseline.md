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
