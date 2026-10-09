# ORDER BY performance fixture

`ordered.db` is the immutable SQLite 3.54.0 fixture for the standalone
ORDER BY performance contract. It contains 65,536 rows with 256-byte payloads
and only one `(score DESC)` index, so all cases except the explicit
index-compatible baseline must consume every source row and use an ordering
capability.

Regenerate it only with the pinned amalgamation and shared library:

```sh
python3 tools/order_by_performance.py create-fixture \
  --profile tests/compatibility/sqlite-oracle-profile-v1.json \
  --sqlite-library build/benchmark/libmodern_sqlite_benchmark_sqlite_shared.dylib \
  --sqlite-c /path/to/pinned/sqlite3.c \
  --sqlite-h /path/to/pinned/sqlite3.h \
  --sql tests/fixtures/order_by_performance/ordered.sql \
  --output /tmp/ordered.db
```

The regeneration test requires byte-identical output before the committed
fixture or workload metadata may change.
