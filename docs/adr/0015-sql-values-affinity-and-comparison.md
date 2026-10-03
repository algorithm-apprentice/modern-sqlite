# ADR-0015: SQL Values, Affinity, and Comparison

- Status: Accepted
- Date: 2026-10-03

## Context

Record decoding, expressions, functions, B-tree keys, and VM registers need
one shared representation of SQLite storage classes. SQLite distinguishes
loss-avoiding affinity from forced `CAST` conversion, compares integers and
reals without first rounding both to `double`, orders storage classes
deterministically, and treats ordinary comparisons with NULL differently from
`IS` and `IS NOT`.

The pinned SQLite 3.54.0 behavior is defined by `src/vdbe.c` for affinity,
`src/vdbemem.c` for casts and numeric coercion, `src/vdbeaux.c` for value
ordering, and `src/util.c` for decimal conversion.

## Decision

- `SqlValue` represents NULL, signed 64-bit INTEGER, IEEE 754 binary64 REAL,
  owned UTF-8 text bytes, or owned blob bytes.
- Values are move-only. Text and blob copies require explicit construction or
  `Clone()`, preventing accidental payload copies in VM and storage hot paths.
- Constructing a REAL from NaN produces NULL, matching SQLite. Positive and
  negative infinity remain REAL values.
- Text preserves embedded NUL and malformed UTF-8 bytes. Blob-to-text and
  text-to-blob casts preserve the exact bytes in the current UTF-8-only
  runtime.
- NONE and BLOB affinity are no-ops. TEXT affinity formats numeric values.
  INTEGER and NUMERIC affinity convert only complete decimal spellings and
  prefer exact integers. REAL affinity accepts the same complete spellings but
  materializes a REAL result.
- Affinity recognizes SQLite ASCII whitespace and decimal syntax only.
  Hexadecimal-looking text such as `0x10` is not converted.
- Forced casts follow SQLite's prefix rules: INTEGER consumes a decimal
  integer prefix and saturates overflow; REAL consumes a decimal floating
  prefix and returns 0.0 when no prefix exists; NUMERIC chooses INTEGER or
  REAL using SQLite's integer-range and exact 51-bit round-trip rules.
- Decimal-to-binary and binary-to-decimal conversion use a C++23 adaptation
  of the pinned SQLite 3.54.0 integer algorithms. This avoids locale and
  standard-library formatting differences and preserves SQLite's documented
  approximately 19-significant-digit input behavior.
- Casting an existing INTEGER or REAL to NUMERIC is a no-op. This intentionally
  differs from applying numeric affinity to a REAL.
- REAL-to-text formatting uses SQLite's locale-independent 17-significant-
  digit rendering, retains `.0` for integer-looking reals, and uses SQLite's
  `Inf` spelling.
- Default ordering is NULL, numbers, text, then blobs. Integer/real comparison
  uses range-aware mixed arithmetic so values beyond binary64's exact integer
  range are not compared after lossy integer-to-real conversion.
- Text and blob ordering is unsigned bytewise order with shorter prefixes
  first. The next collation node replaces only text ordering when a collation
  is supplied.
- Ordinary comparison with either operand NULL yields SQL NULL. `IS` and
  `IS NOT` compare NULLs as values and otherwise use the same storage-class
  ordering and equality.

## Consequences

- Record, B-tree, function, planner, and VM modules share one ownership and
  conversion contract.
- NULL propagation and total key ordering are separate, explicit operations.
- Numeric and comparison hot paths allocate no memory. Allocation occurs only
  when text or blob ownership or formatting requires it.
- UTF-16 conversion, collated text comparison, arithmetic operators, and
  public C API lifetime rules remain in their later DAG nodes.
