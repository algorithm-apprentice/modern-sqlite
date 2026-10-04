# SQLite 3.54.0 Read Session Fixtures

`sqlite-3.54.0-session-v1.db` is generated from `schema-v1.sql` by the
pinned SQLite 3.54.0 shell at Fossil check-in
`65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.

`sqlite-3.54.0-session-v2.db` starts as an exact copy of v1 and then applies
`schema-v2.sql` with the same shell. This preserves physically short v1
records while changing the schema cookie and `SELECT *` metadata for
automatic-reprepare tests.

Both fixtures use 512-byte pages and pass `PRAGMA integrity_check`.

SHA-256:

```text
0060f5837ca41bc54eb070e4077ce7c9509cb44f596e35c50c95be0e67671445  sqlite-3.54.0-session-v1.db
21b2acae3e9afc356c3cab53ec5cf29b61bb039c15a5e811be4697dab87f2079  sqlite-3.54.0-session-v2.db
```
