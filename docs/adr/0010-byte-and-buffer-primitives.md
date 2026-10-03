# ADR-0010: Byte and Buffer Primitives

- Status: Accepted
- Date: 2026-10-03

## Context

Persistent formats, VFS operations, pages, records, journals, and diagnostics
all need a common byte vocabulary. Raw pointer/length pairs make ownership,
mutability, range validation, and copy cost implicit. A heavyweight wrapper
around every view would add hot-path indirection without improving ownership.

The general project Error and Result types are implemented by the next DAG
node, so this prerequisite cannot depend on them.

## Decision

- `ByteView` and `MutableByteView` are zero-overhead aliases of const and
  mutable `std::span<std::byte>`.
- `ByteOffset` and `ByteCount` are distinct explicit strong types over
  `std::size_t`.
- Checked slice functions validate with subtraction after checking the offset,
  avoiding overflow in `offset + length`.
- Range and copy failures return `std::expected` with a local `ByteError`.
  They will not depend on the later general Error type.
- `ByteBuffer` owns a `std::vector<std::byte>`, is move-only, and exposes
  explicit `CopyOf` and `Clone` operations for potentially expensive copies.
- Text conversion functions preserve embedded NUL bytes and do not allocate.
- Copying between views uses `memmove` semantics so overlapping source and
  destination ranges are well-defined.
- Empty operations avoid calling C memory functions with null pointers.

## Consequences

- Storage code can express ownership, borrowing, mutability, offsets, and
  lengths in signatures.
- Non-owning views remain the same size and performance model as
  `std::span`.
- Callers must make ownership copies explicit.
- Allocation failure continues to use the standard exception mechanism until
  an API boundary converts it according to ADR-0004.
- Local byte failures remain narrow and can later be translated into broader
  engine errors without coupling this base module upward.
