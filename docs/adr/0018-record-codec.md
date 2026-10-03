# ADR-0018: SQLite Record Codec

- Status: Accepted
- Date: 2026-10-03

## Context

SQLite table payloads and index keys use the same record format: a
header-size varint, one serial-type varint per field, and a body containing
the field payloads in order. The B-tree and virtual machine both need this
format, but neither should own its representation or comparison rules.

The codec is a persistent-format boundary. It must accept SQLite-compatible
varints, reject malformed external bytes before unchecked access, preserve
arbitrary TEXT and BLOB bytes, and expose index comparison without allocating
owned strings or blobs on the seek path.

SQLite schema formats 1 through 3 encode integer values 0 and 1 as ordinary
one-byte integers. Schema format 4 adds zero-byte serial types 8 and 9. Serial
types 10 and 11 are reserved for SQLite's internal transient uses and never
appear in a well-formed persistent database record.

## Decision

- The record codec is a pure `format` module. It performs no filesystem,
  pager, B-tree, virtual-machine, affinity, or schema-default work.
- `RecordSchemaFormat` represents SQLite schema formats 1 through 4.
  Encoding and validation always receive the applicable schema format.
- `RecordSerialType` exposes the selected serial-type code, payload length,
  and nominal SQL storage class.
- Encoding uses two passes:
  - determine every serial type and the exact header/body sizes; then
  - allocate one `ByteBuffer` and write the complete record.
- INTEGER payloads use the smallest SQLite signed width: 1, 2, 3, 4, 6, or
  8 bytes. Schema format 4 uses serial types 8 and 9 for 0 and 1.
- REAL payloads preserve their IEEE 754 binary64 bit pattern in big-endian
  order. Decoded NaN values become SQL NULL, matching SQLite.
- TEXT and BLOB payloads are byte-preserving. The current engine supports
  UTF-8 databases only, but record decoding does not validate, normalize, or
  terminate TEXT bytes.
- `RecordView` borrows a complete encoded record and validates it once.
  `RecordCursor` then walks its fields sequentially without allocation.
  `RecordFieldView` borrows TEXT/BLOB payloads and stores numeric values
  inline. The input bytes must outlive all derived views and cursors and must
  remain unchanged while any of them are used.
- `DecodeRecord` is the convenience path that materializes owned
  `SqlValue` objects. It allocates only for the result vector and owned
  TEXT/BLOB payloads.
- Persistent-record validation requires:
  - at least one field;
  - a header size that includes and does not precede its own varint;
  - serial-type varints wholly contained in the declared header;
  - no more than 65,534 record fields, covering SQLite's hard
    `2 * SQLITE_MAX_COLUMN` maximum for secondary indexes on `WITHOUT ROWID`
    tables;
  - a header no larger than SQLite's 98,307-byte structural maximum;
  - serial types 8 and 9 only with schema format 4;
  - rejection of reserved serial types 10 and 11;
  - every field payload wholly contained in the record; and
  - exact body consumption with no trailing bytes.
- SQLite-compatible overlong varints remain accepted when all structural
  invariants hold. Encoders always emit canonical varints.
- Malformed external bytes return `ErrorCode::kCorruption`. Invalid caller
  configuration returns `ErrorCode::kMisuse`, out-of-range field access
  returns `ErrorCode::kOutOfRange`, and unrepresentable output sizes return
  `ErrorCode::kTooLarge`.
- Index comparison accepts:
  - a validated encoded record;
  - a borrowed unpacked search-key prefix;
  - one collation, direction, and NULL-placement contract per compared
    field; and
  - an explicit result to use when the entire search-key prefix is
    equivalent.
- Index comparison returns both the requested ordering and whether an
  equivalent prefix was observed. This preserves SQLite's `default_rc` and
  `eqSeen` behavior without importing VDBE or B-tree state.
- A rowid or WITHOUT ROWID primary-key suffix is an ordinary trailing record
  field. Callers compare or omit that suffix by choosing the search-key
  prefix length; the codec has no rowid-specific policy.
- Missing trailing table columns and their schema defaults are handled by
  catalog/row materialization, not by the record codec.

## Consequences

- B-tree lookup and VM column extraction share one independently fuzzable
  format contract without a dependency cycle.
- Validated record iteration and index comparison perform no heap allocation.
- Random field lookup is linear in the field index; hot consumers should use
  `RecordCursor` for one sequential pass.
- Eager decoding is intentionally more expensive because it creates
  independent ownership.
- Persistent files containing serial types 10 or 11 are rejected even though
  SQLite may use those codes in release-specific transient records.
- UTF-16 database decoding, lazy zero-blob storage, affinity application,
  schema-default substitution, and transient no-change records remain in
  later nodes.
