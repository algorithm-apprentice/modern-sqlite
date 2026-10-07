# Transaction Coordinator Node 37 Baseline

## Scope

This is the pre-optimization fixed-work baseline for
`implement-transaction-coordinator`. It is diagnostic evidence, not a matched
SQLite timing claim. Matched write-performance comparison remains Node 40.

## Configuration

- Build: local Debug
- Storage: deterministic 512-byte-page memory VFS
- Journal mode: rollback DELETE semantics
- Cache: 64 pages for the model trace
- Reference model: `std::map<std::int64_t, std::byte>`
- Test:
  `TransactionCoordinatorModel.MixedTransactionsMatchReferenceMap`

## Fixed work

The trace executes 120 explicit transaction cycles:

- 360 write statements;
- 120 named savepoints;
- 120 named savepoint releases;
- 30 `ROLLBACK TO` operations;
- 40 anonymous statement rollbacks;
- 96 outer commits; and
- 24 outer rollbacks.

Each cycle performs:

1. one insert-or-replace statement;
2. one named savepoint;
3. one insert-or-replace statement that may roll back independently;
4. release or rollback-to of the named savepoint;
5. one delete statement; and
6. outer commit or rollback.

The final committed table contains 18 rows. The exact row/value vector is:

```text
1:94,4:90,6:108,7:117,9:104,10:113,11:220,13:109,14:118,
16:105,17:114,20:110,24:115,25:210,28:120,29:98,30:107,31:219
```

The final ordered Modern B-tree scan matches the reference map exactly.

## Local diagnostic result

On the recording machine, the Debug test body reported approximately 11 ms
for the fixed trace. This number is not a regression threshold and is not
comparable to SQLite until Node 40 supplies matched schema, durability, cache,
journal, corpus, and operation configuration.

## Correctness evidence paired with this baseline

- implicit write commit;
- explicit multi-statement commit;
- statement rollback after pre-rollback database spill;
- named savepoint rollback followed by outer commit;
- transaction-savepoint release;
- full transaction rollback after a spilled mutation;
- every persistent mutation cut in sync-only and immediately durable modes;
- exact old-or-committed image classification after two Modern reopen cycles
  for five commit-capable flows;
- exact original-image restoration for the full-rollback flow;
- pinned SQLite 3.54.0 `PRAGMA integrity_check`;
- complete blob-byte, logical row, and operation-specific freelist validation;
- exhaustive coordinator-owned allocation failure; and
- deterministic savepoint publication and rollback I/O failure matrices.
