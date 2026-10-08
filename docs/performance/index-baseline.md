# Index Performance Baseline

ADR-0052 defines a separate version-1 indexed workload contract so the
existing read and write baselines remain immutable.

The pre-feature baseline intentionally compares Modern SQLite's table-scan
fallback with pinned SQLite 3.54.0 index plans. A complete validated run may
therefore exit with the severe-regression status before index planning is
implemented.

## Workloads

`tests/performance/index-workloads-v1.json` pins eight cases over one
SQLite-created 4,096-row fixture:

1. covering equality hit;
2. covering equality miss;
3. noncovering equality hit;
4. two-column covering equality;
5. covering two-sided range;
6. noncovering two-sided range;
7. deliberately unselective noncovering STAT1 fallback; and
8. the same unselective predicate with a covering result.

The fixture and every logical result vector are byte-reproducible. The
unselective noncovering case must report SQLite full-scan steps; the other
SQLite cases must not.

Each baseline repetition must run for at least 5 milliseconds. This lower
floor is deliberate: scaling an indexed point lookup to the read baseline's
200-millisecond SQLite floor would make Modern's pre-feature table-scan
control impractically long without changing the fixed work.

## Build and smoke validation

Configure the benchmark preset with the pinned amalgamation and build the
existing read timing and diagnostic binaries:

```sh
cmake --preset benchmark \
  -DMODERN_SQLITE_PINNED_SQLITE_C=/absolute/path/to/sqlite3.c \
  -DMODERN_SQLITE_PINNED_SQLITE_H=/absolute/path/to/sqlite3.h
cmake --build --preset benchmark --parallel 3 --target \
  modern_sqlite_read_benchmark \
  modern_sqlite_read_diagnostics \
  modern_sqlite_benchmark_sqlite_shared
```

Run the strict fixture, smoke, diagnostics, and tool contracts:

```sh
ctest --test-dir build/benchmark --output-on-failure \
  -R '^performance\.index_(performance_tool|benchmark_cli|fixture_regeneration)$'
```

## Generate and validate evidence

Generation requires a clean committed worktree, exact-source binaries, and an
absent output directory:

```sh
python3 tools/index_performance.py generate-baseline \
  --repository-root . \
  --workloads tests/performance/index-workloads-v1.json \
  --timing-binary build/benchmark/modern_sqlite_read_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_read_diagnostics \
  --sqlite-c /absolute/path/to/sqlite3.c \
  --sqlite-h /absolute/path/to/sqlite3.h \
  --output benchmarks/index-baseline-v1
```

The schedule contains 48 timing children and 16 diagnostic children. Exit
code `2` means the complete baseline is structurally valid but one or more
10x wall/CPU guards fail. Exit code `1` means a harness, provenance,
correctness, timeout, process, or artifact failure.

Offline validation does not rerun timings:

```sh
python3 tools/index_performance.py validate-baseline \
  --repository-root . \
  --workloads tests/performance/index-workloads-v1.json \
  --baseline benchmarks/index-baseline-v1
```

The baseline is regenerated only after the complete index-planning node is
integrated. Workload SQL, fixture bytes, STAT1 contents, iteration counts,
cache/page settings, result digests, and guard thresholds remain unchanged.

## Canonical pre-feature result

The committed `benchmarks/index-baseline-v1` run was generated from clean
source revision `c9e8b4303314ac35f98294c5ffd2de8b4560dc04`.

It records:

| Case | Wall ratio | CPU ratio | Maximum round wall |
|---|---:|---:|---:|
| covering equality hit | 226.84x | 226.84x | 233.61x |
| covering equality miss | 231.15x | 231.10x | 236.42x |
| noncovering equality hit | 203.10x | 203.07x | 204.28x |
| two-column covering equality | 222.45x | 222.44x | 224.92x |
| covering range | 155.11x | 155.10x | 158.47x |
| noncovering range | 60.44x | 60.44x | 61.15x |
| unselective noncovering control | 3.00x | 3.00x | 3.09x |
| unselective covering control | 13.92x | 13.91x | 14.28x |

The result is a validated guard failure, not a threshold change. It confirms
three distinct acceptance targets:

- bounded equality and range seeks must replace full table scans;
- noncovering access must add only the required table rowid lookup; and
- STAT1 must preserve the table-scan choice for the deliberately unselective
  noncovering case while allowing the corresponding covering plan.

All 48 timing children and 16 diagnostic children completed with exact logical
work, source/build/host provenance, and empty stderr artifacts.
