# Write Performance Baseline

ADR-0050 defines the public-session write benchmark. ADR-0113 adds the
authoritative version-2 contract, which compares Modern SQLite with pinned
SQLite 3.54.0 under separate `engine-default` and `matched-durable` profiles
while preserving version 1 as historical evidence.

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
python3 tools/write_performance_v2.py generate-baseline \
  --repository-root . \
  --workloads tests/performance/write-workloads-v2.json \
  --timing-binary build/benchmark/modern_sqlite_write_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_write_diagnostics \
  --output benchmarks/write-baseline-v2
```

Exit code `2` means the complete structurally valid baseline was written but
the matched-durable 10x guard failed. Do not delete or replace such a run to
obtain a favorable sample. Exit code `1` means a harness, provenance,
correctness, process, timeout, or artifact failure.

Offline validation never reruns timing:

```sh
python3 tools/write_performance_v2.py validate-baseline \
  --repository-root . \
  --workloads tests/performance/write-workloads-v2.json \
  --baseline benchmarks/write-baseline-v2
```

`tools/write_performance.py`, `tests/performance/write-workloads-v1.json`,
and `benchmarks/write-baseline-v1/` remain frozen together. They document the
original profile semantics and are not regenerated after version 2 becomes
available. Validate their recorded artifacts without binding them to the
current benchmark source:

```sh
python3 tools/write_performance_v2.py validate-historical-baseline \
  --repository-root . \
  --workloads tests/performance/write-workloads-v1.json \
  --baseline benchmarks/write-baseline-v1
```

## Historical version-1 findings

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

The original baseline therefore recorded a validated guard failure instead of
widening the threshold or replacing samples. ADR-0008's
optimization-admission process admitted ADR-0051 as a separate reviewed node.

## Historical version-1 canonical result

The regenerated `benchmarks/write-baseline-v1` run at source revision
`83c1344c53d756ca4e0d14dbf33db5e6c8c25872` records:

- `delete-scan-implicit`: 9.08x aggregate wall and 7.62x CPU, with maximum
  paired-round ratios of 9.20x wall and 7.75x CPU;
- `update-scan-implicit`: 6.93x aggregate wall and 5.70x CPU, with maximum
  paired-round ratios of 7.02x wall and 5.77x CPU; and
- every matched aggregate and every paired-round wall/CPU ratio below 10x.

`engine-default` remains informational. ADR-0051's SQLite-aligned one-pass
scan-mutation implementation preserves the version-1 workloads and matched
durability configuration while satisfying the canonical guard.

## Version-2 baseline result

The authoritative `benchmarks/write-baseline-v2` run was generated from
source revision `362c4f32dc7f78125c1a92bef83965b74baffce5`, tree
`bdf9697336ac4b19881c619011fbe75b99287868`. Every matched aggregate and
paired round remains below the 10x guard.

The corrected profile configuration and statement-journal diagnostics are:

- `engine-default` reports and opens Modern with `temp_store=file`;
- `matched-durable` reports and opens Modern with `temp_store=memory`; and
- both profiles record zero subjournal VFS opens, writes, and bytes for
  `insert-batch-explicit`.

The matched `insert-batch-explicit` result improves from the historical
version-1 7.058144x wall and 7.342122x CPU to 5.556505x wall and 5.779553x
CPU. Its subjournal work falls from one open, 74,481 writes, and 305,372,100
bytes to zero for all three counters. The remaining matched scan and batch
ratios are:

| Case | Wall ratio | CPU ratio |
|---|---:|---:|
| `update-scan-implicit` | 6.969554x | 5.598886x |
| `delete-scan-implicit` | 8.519555x | 7.082721x |
| `mixed-batch-commit` | 5.741514x | 4.802254x |
| `mixed-batch-rollback` | 4.931226x | 4.140400x |

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
