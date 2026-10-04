# SQLite 3.54.0 Catalog Loader Fixture

`sqlite-3.54.0-catalog.db` was generated from `schema.sql` by the pinned
SQLite 3.54.0 shell at Fossil check-in
`65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
It uses 512-byte pages and passes `PRAGMA integrity_check`.

The fixture covers:

- INTEGER PRIMARY KEY rowid aliases and AUTOINCREMENT;
- descending INTEGER primary keys with automatic indexes;
- STRICT and WITHOUT ROWID table derivation;
- UNIQUE constraint folding and automatic-index ordinals;
- explicit, expression, COLLATE, descending, and partial index terms;
- legacy `GENERATED ALWAYS` declared-type normalization;
- legacy single-quoted indexed terms;
- `sqlite_sequence` and `sqlite_stat1`;
- table, primary-index, automatic-index, and explicit-index statistics; and
- `unordered`, `noskipscan`, and `sz=` STAT1 modifiers.

`sqlite-3.54.0-expression-compatibility.db` was generated from
`expression-compatibility.sql`. It uses SQLite's writable-schema compatibility
mode to retain a legacy DEFAULT variable and an application-defined function
in an expression index. It also covers parenthesized automatic and explicit
index columns, nested COLLATE, rowid references in CHECK and partial-index
expressions, and a known function with deferred DEFAULT arity checking. The
loader normalizes the variable to NULL and preserves the unknown function for
later connection-owned resolution.

`sqlite-3.54.0-invalid-expression.db` was generated from
`invalid-expression.sql`. Its index definition names a nonexistent column and
must be rejected as persistent-schema corruption.

`sqlite-3.54.0-invalid-function.db` was generated from
`invalid-function.sql`. It calls the known `lower` function with the wrong
arity and must be rejected as persistent-schema corruption.

`sqlite-3.54.0-duplicate-automatic-root.db` was generated from
`duplicate-automatic-root.sql`. It repeats the same blank automatic-index row;
SQLite applies both rows in scan order and the final root remains valid.

`sqlite-3.54.0-stat1-shape.db` was generated from `stat1-shape.sql`. Its
`sqlite_stat1` table has four reordered columns, and its `sz=` value exceeds
the signed 32-bit range. The loader selects fields by name, ignores the extra
column, and clamps SQLite's failed 32-bit parse to two.

SHA-256:

```text
f996c70be3c2b1377e16987a4a34ac19735fc83bc73a9951f9f362d72cbf3608  sqlite-3.54.0-catalog.db
222f7b58a4ca11fca2b4b02dae0e7ea5e322131549187dc027307867bad0ad41  sqlite-3.54.0-expression-compatibility.db
ff92bcccf4c4b7684bb8825ee70f6dffc279073bf72fa74c69bb1bf87614898d  sqlite-3.54.0-invalid-expression.db
0a5e49f2d1bca32f1e5b3f75c940f727853d8fec39f1f75425df56b80dcefe5e  sqlite-3.54.0-invalid-function.db
71aba1f4bd4a189c60a544082f3831cc5aeaa19672e5c5f5b3171d9a5185aa4d  sqlite-3.54.0-duplicate-automatic-root.db
5c14ac9e523e7ef6238ef15d45a1d7d522118198ced8a96f363864df79a80c4f  sqlite-3.54.0-stat1-shape.db
```
