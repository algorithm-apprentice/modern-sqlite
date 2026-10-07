# Node 36 B-Tree Write Baseline

## Scope

This is the pre-optimization fixed-work baseline for
`implement-btree-write`. It records deterministic structural work and
correctness outcomes. It does not claim a wall-clock advantage over SQLite;
the matched SQL-level write benchmark belongs to
`establish-write-performance-baseline` after the writable MVP harness and
transaction/DML layers exist.

## Configuration

- Page size: 512 bytes
- Cache capacity: 64 pages
- Journal mode: rollback journal, DELETE finalization
- Tree types: one rowid table and one index B-tree
- Transaction: one active pager write transaction
- Workload: 300 deterministic mixed operations over a 97-key domain
- Table operations: insert-only, replace, and delete with eight-byte payloads
- Index operations: complete-key insert and delete

The executable specification is
`BtreeWriterModel.MixedTableAndIndexOperationsMatchReferenceContainers` in
`tests/unit/storage/btree/writer_test.cpp`.

## Baseline result

- Table rows after 300 operations: 73
- Index records after 300 operations: 32
- Ordered table scan: byte-identical to the reference `std::map`
- Ordered index scan: value-identical to the reference `std::set`
- Table clear count: 73
- Index clear count: 32
- Local Debug diagnostic runtime: approximately 13 ms on Apple Silicon

The runtime is diagnostic only. Future performance comparisons must match
SQLite page size, cache size, journal mode, synchronous mode, mmap, temporary
storage, schema, corpus, transaction boundaries, and final-content checks.
