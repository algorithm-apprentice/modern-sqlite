# SQLite 3.54.0 Read Lowering Fixture

`sqlite-3.54.0-alter-defaults.db` is generated from `alter-defaults.sql` by
the pinned SQLite 3.54.0 shell at Fossil check-in
`65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
It uses 512-byte pages and passes `PRAGMA integrity_check`.

The fixture covers physically short records created before `ALTER TABLE ADD
COLUMN`, affinity-applied literal defaults, explicit stored NULL values, and
both ordinary rowid and WITHOUT ROWID table layouts.

SHA-256:

```text
ce020e691a8ba01d0ec452bcf53a0e0f4666ac71a6bb4be2d0b73104974bc72f  sqlite-3.54.0-alter-defaults.db
```
