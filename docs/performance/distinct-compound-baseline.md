# DISTINCT and Compound SELECT Performance Baseline

ADR-0056 defines a separate version-1 performance contract for DISTINCT,
VALUES, and compound SELECT so the general read, index, write, and standalone
ORDER BY baselines remain immutable.

The contract reuses the SQLite-created 65,536-row ORDER BY fixture without
changing its bytes. The workload manifest is
`tests/performance/distinct-compound-workloads-v1.json`; committed evidence is
stored in `benchmarks/distinct-compound-baseline-v1/`.

## Workloads

The contract pins fourteen cases:

1. low-cardinality DISTINCT over two integer values;
2. high-cardinality DISTINCT over every category;
3. NOCASE-collated DISTINCT with comparator-equivalent text;
4. DISTINCT followed by ORDER BY and LIMIT;
5. a 256-row VALUES arm combined with a table arm under ordered UNION ALL
   LIMIT;
6. file-mode UNION with INTEGER/REAL comparator-equivalent replacement;
7. EXCEPT membership;
8. INTERSECT membership;
9. multi-arm ordered UNION ALL with per-arm top-N;
10. ordered UNION full-key merge;
11. ordered EXCEPT merge;
12. ordered INTERSECT merge;
13. a five-arm mixed left-associated compound; and
14. a file-mode keyed-relation baseline retained for later optimization
    comparisons.

Every case fixes the SQL text, source-row work, result rows and digest,
temporary-store mode, sorter threshold, and warmup/measured/diagnostic
iterations. The logical work and digest must match between Modern SQLite and
the pinned SQLite 3.54.0 build before timing is accepted.

## Matched configuration

Both engines use:

- 4,096-byte pages;
- a 512-page cache;
- rollback-journal DELETE mode;
- synchronous FULL;
- mmap disabled;
- one thread and zero worker threads;
- query-only execution;
- the case-specific memory or file temporary store; and
- the case-specific sorter memory threshold.

The fixture identity, schema, row count, payload size, SQL source, SQLite
profile, compile options, compiler flags, linker flags, executable hashes,
source revision, Git tree, and worktree state are recorded in the run
manifest.

## Guard

The version-1 admission guard requires every aggregate and every paired-round
wall/CPU ratio to remain at or below 40x SQLite. Timing samples below five
milliseconds are rejected. This first contract intentionally records the
pre-optimization keyed-relation and high-cardinality DISTINCT gaps named by
ADR-0056 rather than disguising them with easier inputs. Later optimization
work must preserve the immutable workload and fixture identities and may
tighten the guard only through a new reviewed contract version.

## Build and smoke validation

Configure the benchmark build with the pinned SQLite amalgamation:

```sh
cmake --preset benchmark \
  -DMODERN_SQLITE_PINNED_SQLITE_C=/absolute/path/sqlite3.c \
  -DMODERN_SQLITE_PINNED_SQLITE_H=/absolute/path/sqlite3.h
cmake --build --preset benchmark
```

Validate the manifest and run the matched smoke/diagnostic matrix:

```sh
python3 tools/distinct_compound_performance.py validate-workloads \
  --repository-root . \
  --workloads tests/performance/distinct-compound-workloads-v1.json

python3 tests/performance/distinct_compound_benchmark_cli_test.py \
  --timing-binary build/benchmark/modern_sqlite_distinct_compound_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_distinct_compound_diagnostics \
  --repository-root . \
  --workloads tests/performance/distinct-compound-workloads-v1.json
```

## Baseline generation

Baseline generation requires a clean committed source tree and a new output
path:

```sh
python3 tools/distinct_compound_performance.py generate-baseline \
  --repository-root . \
  --workloads tests/performance/distinct-compound-workloads-v1.json \
  --timing-binary build/benchmark/modern_sqlite_distinct_compound_benchmark \
  --diagnostic-binary build/benchmark/modern_sqlite_distinct_compound_diagnostics \
  --sqlite-c /absolute/path/sqlite3.c \
  --sqlite-h /absolute/path/sqlite3.h \
  --output benchmarks/distinct-compound-baseline-v1
```

Validate committed evidence with:

```sh
python3 tools/distinct_compound_performance.py validate-baseline \
  --repository-root . \
  --workloads tests/performance/distinct-compound-workloads-v1.json \
  --baseline benchmarks/distinct-compound-baseline-v1
```

The output directory is immutable evidence: aggregate, run manifest, raw
timing reports, raw diagnostic reports, and bounded stderr logs. Regeneration
uses a new directory rather than modifying an existing baseline in place.
