# ADR-0013: Binary Coding Primitives

- Status: Accepted
- Date: 2026-10-03

## Context

SQLite database pages, records, rollback journals, and WAL files depend on
small binary-format operations that are both compatibility-sensitive and hot.
Unsafe pointer arithmetic or host-endian assumptions would spread format
knowledge and corruption risks through every storage module.

The pinned SQLite 3.54.0 behavior is defined by `src/util.c` for endian coding,
varints, and signed overflow checks, by `src/pager.c` for rollback-journal page
checksums, and by `src/wal.c` for WAL checksums.

## Decision

- Fixed-width SQLite integers use unaligned-safe big-endian load and store
  templates for unsigned 16-, 32-, and 64-bit values.
- Trusted callers use fixed-extent spans. Checked dynamic-span wrappers reject
  short input or output before reading or mutating memory.
- SQLite varint decoding accepts non-canonical overlong encodings just as
  SQLite does, consumes at most nine bytes, treats the ninth byte as eight data
  bits, and rejects truncated input without an out-of-bounds read.
- SQLite varint encoding always emits the shortest canonical representation
  and leaves an undersized destination unchanged.
- Checked signed 64-bit addition, subtraction, and multiplication are
  functional operations: overflow returns an error and no wrapped value.
- Rollback-journal and WAL checksum helpers reproduce SQLite's modulo-2^32
  algorithms. WAL word byte order is explicit rather than inferred from the
  host.
- `CodingError` is a small module-local enum used with `std::expected`.
  Storage boundaries map it to engine `Error` values, avoiding a large,
  allocating error alternative in hot primitive results.
- Golden vectors are independent constants derived from the pinned format
  algorithms, not round trips through the implementation under test.

## Consequences

- Format readers can validate bounds once and then use fixed-extent hot-path
  loads.
- Modern SQLite remains compatible with permissive SQLite varint reads while
  producing canonical output.
- Arithmetic overflow and malformed binary input are explicit and cannot
  become wrapped values or partial writes.
- Later record, pager, rollback-journal, and WAL modules share one tested
  implementation of their primitive format rules.
