# Pinned Write-Performance Fixtures

These SQLite 3.54.0 databases are immutable inputs for ADR-0050.

- `schema.db` contains only the canonical `kv` table.
- `populated.db` contains 65,536 deterministic 256-byte values.
- both use 4096-byte pages, UTF-8, no auto-vacuum, user version 1, and
  rollback-journal DELETE mode.

Generate the pinned amalgamation and shared benchmark library, then regenerate
one fixture into a new path:

```sh
python3 tools/write_performance.py regenerate-fixture \
  --profile tests/compatibility/sqlite-oracle-profile-v1.json \
  --sqlite-library build/benchmark/libmodern_sqlite_benchmark_sqlite_shared.dylib \
  --sqlite-c /absolute/path/to/sqlite3.c \
  --sqlite-h /absolute/path/to/sqlite3.h \
  --sql tests/fixtures/write_performance/schema.sql \
  --fixture-id schema \
  --output /tmp/schema.db
```

Use `populated.sql`, `populated`, and a distinct output path for the populated
fixture. The benchmark CTest regenerates both databases, compares metadata,
and requires byte-identical output.

Canonical hashes:

| Artifact | SHA-256 |
|---|---|
| `schema.sql` | `c22a5e309a27d04810ebbe9ad26a0e8d66c39f1b544aeb3ad6407be7083fcce0` |
| `schema.db` | `26c59e7c83f4ee09a8a341a54abefb88f381e7fc110e8e63c8df661643eee3fa` |
| `populated.sql` | `e495713f888ebb010b9b5cab12565014fbe5e9dba0cdf0e56f7114ae7da8caf1` |
| `populated.db` | `81fa90c6ac9c3ab20f24f3d08823c564d6203e09f316a397290cdcd546eb0091` |

Both databases must reopen in pinned SQLite and Modern SQLite. Pinned SQLite
must return `ok` from `PRAGMA integrity_check`.
