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

Every case fixes the SQL text, compound-input-row work, result rows and
digest, temporary-store mode, sorter threshold, and
warmup/measured/diagnostic iterations. Every work object, including smoke and
fixture verification, is compared with an independent immutable constant.
The logical work and digest must match between Modern SQLite and the pinned
SQLite 3.54.0 build before timing is accepted.

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

## Diagnostic evidence

Diagnostic builds add a non-deterministic identity function named
`modern_sqlite_probe`. The dedicated diagnostic SQL wraps one projected
expression for every input row of every compound arm. Every arm has a distinct
tag, every VALUES row has its own tag, and collection starts after warmup.
Modern and SQLite must report the exact pinned `probe_counts` vector and its
`source_rows` sum, so compensating skipped and repeated arms cannot pass. The
probe is absent from timing SQL, the timing binary, and ordinary production
builds.

Because every Modern workload is query-only, a diagnostic database-page
write can only belong to temporary storage. The file-mode keyed-relation case
must report positive Modern page writes and positive SQLite temporary-spill
bytes. The smaller replacement-UNION case must report zero for both and acts
as the no-spill control.

## Canonical baseline result

The committed `benchmarks/distinct-compound-baseline-v1` run was generated
from clean source revision `443df74f982ca2dcaf86ad2ef9e4c0b7488f2e7b`
with tree `56c8749e717addf74e18dfd5072095173e90bc29`.

All fourteen cases pass the matched 40x wall and CPU guards in aggregate and
in every paired round:

| Case | Wall ratio | CPU ratio | Maximum round wall |
|---|---:|---:|---:|
| low-cardinality DISTINCT | 4.05x | 4.04x | 4.07x |
| high-cardinality DISTINCT | 26.55x | 26.52x | 27.93x |
| collated DISTINCT | 4.29x | 4.29x | 4.32x |
| DISTINCT ORDER BY LIMIT | 9.39x | 9.39x | 9.51x |
| VALUES UNION ALL LIMIT | 11.31x | 11.30x | 11.49x |
| file-mode replacement UNION | 17.58x | 17.58x | 18.04x |
| EXCEPT membership | 0.96x | 0.96x | 1.01x |
| INTERSECT membership | 1.02x | 1.02x | 1.03x |
| ordered three-arm UNION ALL | 5.43x | 5.43x | 5.57x |
| ordered UNION merge | 8.29x | 8.30x | 8.42x |
| ordered EXCEPT merge | 1.10x | 1.10x | 1.12x |
| ordered INTERSECT merge | 1.09x | 1.09x | 1.09x |
| five-arm mixed compound | 2.16x | 2.16x | 2.22x |
| file-mode keyed-relation baseline | 12.90x | 13.00x | 13.04x |

High-cardinality DISTINCT is the largest recorded gap, with maximum
paired-round ratios of `27.925789x` wall and `27.891464x` CPU. The file-mode
keyed-relation case records 95,282 Modern temporary page writes and
`18,284,574` SQLite temporary spill bytes. The replacement-UNION control
records zero for both.

All 84 timing children and 28 diagnostic children completed with exact
per-tag input work, 252 accepted timing samples, source/build/host provenance,
and empty stderr artifacts.

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
CI and CTest validate this directory unconditionally, so missing evidence is
a failure rather than a skipped check.
