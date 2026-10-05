# Pinned Read-Performance Baseline

The version-1 read baseline compares the public Modern SQLite read API with
SQLite 3.54.0 under the immutable workload contract in
`tests/performance/read-workloads-v1.json`. Timing evidence is generated only
from a clean source commit. Hosted CI validates committed evidence and runs the
smoke contract; it does not regenerate performance ratios.

## Build

Generate the pinned SQLite amalgamation first, then configure the benchmark
build with the exact source and header:

```sh
cmake --preset benchmark \
  -DMODERN_SQLITE_PINNED_SQLITE_C=/absolute/path/to/sqlite3.c \
  -DMODERN_SQLITE_PINNED_SQLITE_H=/absolute/path/to/sqlite3.h
cmake --build --preset benchmark
```

The configure step rejects any source or header whose SHA-256 does not match
the pinned profile. The authoritative timing executable links pinned SQLite
statically and an ordinary uninstrumented Modern SQLite engine. The diagnostic
executable links a separately compiled instrumented Modern SQLite engine. Both
executables embed the Git revision and tree seen during configuration.
Reconfigure after committing implementation changes; generation rejects stale
binary source identities, a mismatched `CMAKE_HOME_DIRECTORY`, unoptimized
flags, sanitizers, coverage, profiling instrumentation, and LTO. The generator
also validates and records normalized flags from the actual Ninja compile and
link commands, not only CMake cache variables.

Run the deterministic smoke, identity, diagnostic, and fixture-regeneration
contracts with:

```sh
ctest --test-dir build/benchmark \
  --output-on-failure \
  -R '^performance\.read_(benchmark_cli|fixture_regeneration)$'
```

## Generate and validate evidence

Commit all implementation and documentation changes before generation. The
runner rejects a dirty worktree and an existing output directory:

```sh
python3 tools/read_performance.py generate-baseline \
  --repository-root . \
  --workloads tests/performance/read-workloads-v1.json \
  --timing-binary build/benchmark/modern_sqlite_read_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_read_diagnostics \
  --sqlite-c /absolute/path/to/sqlite3.c \
  --sqlite-h /absolute/path/to/sqlite3.h \
  --output benchmarks/read-baseline-v1
```

The runner creates a fresh database copy for every child, preserves every raw
JSON report and stderr log, records source/build/host provenance, recomputes all
medians and exact ratios, and validates the completed directory before
returning. A validated severe-regression failure preserves the complete report
and exits with status 2.

Validate committed evidence without rerunning timings:

```sh
python3 tools/read_performance.py validate-baseline \
  --repository-root . \
  --workloads tests/performance/read-workloads-v1.json \
  --baseline benchmarks/read-baseline-v1
```

## Selected-case profiling

Collect profiles only after an unprofiled baseline. Profile elapsed time is not
baseline evidence and must not be presented as a speedup or regression.

On macOS, capture an Xcode Time Profiler trace:

```sh
xcrun xctrace record \
  --template 'Time Profiler' \
  --output modern-point-present.trace \
  --launch -- \
  build/benchmark/modern_sqlite_read_benchmark \
  profile modern point-present-ipk-pressure \
  tests/fixtures/read_performance/pressure.db 1048576
```

On Linux, collect counters and a call-graph profile:

```sh
perf stat -- \
  build/benchmark/modern_sqlite_read_benchmark \
  profile modern point-present-ipk-pressure \
  tests/fixtures/read_performance/pressure.db 1048576

perf record --call-graph dwarf -- \
  build/benchmark/modern_sqlite_read_benchmark \
  profile modern point-present-ipk-pressure \
  tests/fixtures/read_performance/pressure.db 1048576
perf report
```

Optional Cachegrind instruction attribution is diagnostic only:

```sh
valgrind --tool=cachegrind \
  build/benchmark/modern_sqlite_read_benchmark \
  profile modern point-present-ipk-fit \
  tests/fixtures/read_performance/fit.db 1048576
```

Replay fixed-work counters separately:

```sh
build/benchmark/modern_sqlite_read_diagnostics \
  run modern point-present-ipk-pressure \
  tests/fixtures/read_performance/pressure.db diagnostic
```

Unsupported profilers must fail explicitly. Do not substitute another capture
mode silently, and never copy profiled durations into the aggregate report.
