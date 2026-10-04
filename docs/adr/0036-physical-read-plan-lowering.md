# ADR-0036: Physical Read Plan Lowering

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0035 publishes immutable physical SELECT plans with one selected access
path and an exact execution-order contract. ADR-0031 publishes typed immutable
bytecode, and ADR-0032 executes verified read programs without depending on
parser, binder, planner, optimizer, or catalog types. The next dependency-graph
node must connect those boundaries:

> Physical read plans lowered into verified bytecode.

The supported statement remains deliberately narrow:

- zero or one source;
- a constant-row, empty, table-scan, or rowid-lookup access leaf;
- optional statement guards;
- optional residual predicates;
- optional LIMIT and OFFSET;
- one projection;
- ordinary rowid tables, WITHOUT ROWID tables, and `sqlite_schema`; and
- the complete bound scalar-expression surface from ADR-0033.

The lowering layer must preserve the evaluation contract already established
by binding, logical planning, and physical optimization:

1. evaluate and coerce LIMIT once;
2. stop successfully on LIMIT zero before OFFSET, guards, key evaluation,
   source initialization, filtering, or projection;
3. evaluate and coerce OFFSET once;
4. evaluate statement guards once in source order;
5. for rowid access, open the cursor, evaluate the selected key once, and
   seek;
6. evaluate residual predicates once per candidate row in source order;
7. apply OFFSET after filtering and before projection;
8. evaluate result expressions from left to right;
9. suspend after publishing each result row; and
10. stop after the requested positive LIMIT while treating a negative LIMIT
    as unlimited and a negative OFFSET as zero.

Pinned SQLite 3.54.0 provides the behavioral reference:

- `src/select.c:891-900`, `src/select.c:1193-1579`,
  `src/select.c:2573-2621`, and `src/select.c:8369-8469` generate
  register-resident LIMIT/OFFSET state, the inner result loop, and the simple
  SELECT path;
- `src/where.c:6990-7045` emits source-independent statement guards;
- `src/wherecode.c:1680-1775` and `src/wherecode.c:2600-2734` generate
  rowid seeks, scans, and residual predicates;
- `src/expr.c:2378-2410` performs SQLite's narrow scalar AND/OR
  simplification;
- `src/expr.c:4686-4710` and `src/expr.c:5780-5850` generate lazy
  coalesce and conditional forms;
- `src/vdbe.c:1806-1840` suspends on a result row;
- `src/vdbe.c:2165-2211` implements strict integer and REAL-affinity
  operations; and
- `src/vdbe.c:5600-5695` implements lossless rowid-seek conversion.

Modern SQLite does not preserve SQLite opcode numbers or mutable VDBE operand
patching, but it must preserve the observable evaluation, coercion, error,
and result order.

The existing bytecode is almost sufficient. It already provides:

- constants, parameters, copies, unary and binary operations;
- affinity, cast, comparison, and scalar-call instructions;
- table and index cursor descriptors;
- rewind, next, exact rowid seek, field reads, and rowid reads;
- labels, jumps, conditional jumps, result suspension, and halt; and
- structural plus definite-state verification.

Two semantic operations are missing.

SQLite LIMIT and OFFSET accept values that losslessly convert to INTEGER and
otherwise fail with `SQLITE_MISMATCH`.
`ApplyAffinityInstruction` is insufficient because an unconvertible value is
left unchanged rather than rejected, while `CastInstruction` is too permissive
because it truncates or synthesizes an integer. Correct lowering therefore
requires one narrow typed instruction corresponding to SQLite's
integer-requirement operation.

SQLite also applies `OP_RealAffinity` after reading a REAL-affinity column.
That operation converts only INTEGER storage to REAL. The existing
`ApplyAffinityInstruction(kReal)` is broader because it also parses numeric
TEXT, which changes results for valid files whose stored value predates a
schema-affinity change or otherwise remains TEXT. Exact read compatibility
therefore requires one integer-only REAL-affinity instruction.

This node must not introduce a general code-generation framework, execute
plans directly, add ordinary-index selection, cache expression results across
occurrences, or move runtime name resolution into the compiler.

## Decision

Add read lowering under:

- `include/modern_sqlite/lowering/read_lowering.hpp`; and
- `src/lowering/read_lowering.cpp`.

Extend the typed bytecode and read VM with:

- `MustBeIntegerInstruction`; and
- `RealAffinityInstruction`.

### Public lowering boundary

The public contract is:

```cpp
enum class ReadLoweringErrorCode : std::uint8_t {
  kInvalidInput,
  kResourceLimit,
  kInternalInvariant,
};

struct ReadLoweringError {
  ReadLoweringErrorCode code =
      ReadLoweringErrorCode::kInternalInvariant;
  std::optional<ProgramError> program_error{};
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view ReadLoweringErrorCodeName(
    ReadLoweringErrorCode code) noexcept;

using LowerReadPlanResult =
    std::expected<BytecodeProgram, ReadLoweringError>;

[[nodiscard]] LowerReadPlanResult LowerReadPlan(
    const PhysicalPlan& plan, ProgramLimits limits = {});
```

Lowering borrows one valid immutable `PhysicalPlan` and returns an independent
move-only `BytecodeProgram`. It does not consume, mutate, or retain the plan.
The caller may keep the physical plan for diagnostics, AI-native explain
output, or later session ownership, while the program owns every execution
constant, symbol, cursor descriptor, result descriptor, and instruction.

A moved-from or otherwise invalid physical plan returns `kInvalidInput`.
Program resource-limit failures return `kResourceLimit` and retain the exact
`ProgramError`. Any other builder, verifier, catalog-shape, physical-shape, or
ID invariant failure returns `kInternalInvariant`; a nested `ProgramError` is
retained when available.

The base error mappings are:

- invalid input -> `ErrorCode::kMisuse`;
- resource limit -> `ErrorCode::kTooLarge`; and
- internal invariant -> `ErrorCode::kInternal`.

SQL-facing parse, bind, and catalog errors have already occurred. Runtime
LIMIT/OFFSET conversion, scalar-function, collation-resolution, storage, and
I/O errors remain VM errors.

`std::bad_alloc` propagates from lowering. The later session compiler is the
allocation boundary that converts it to `Error::OutOfMemory()`, matching the
existing syntax, catalog, binder, planner, optimizer, and bytecode layers.

### Bytecode extensions

#### Strict integer instruction

Add:

```cpp
struct MustBeIntegerInstruction {
  RegisterId input;
  RegisterId output;
};
```

`Instruction`, `InstructionKind`, `InstructionKindOf()`, and
`InstructionKindName()` include the new alternative and stable name
`must_be_integer`. The instruction remains within the existing 32-byte
variant limit.

The verifier:

- validates both registers;
- requires the input to be definitely initialized; and
- marks the output definitely initialized on fallthrough.

The VM evaluates the instruction atomically:

1. clone the input;
2. apply SQLite numeric affinity;
3. accept INTEGER directly;
4. accept a REAL only when it is finite, within the signed 64-bit conversion
   range, and exactly equal to the converted integer;
5. write the resulting INTEGER to the output; or
6. return `ErrorCode::kTypeMismatch` with `datatype mismatch`.

NULL, BLOB, malformed or nonnumeric TEXT, fractional numeric values, and
out-of-range numeric values fail. Numeric TEXT such as `1`, `1.0`, or `1e0`
and exactly integral REAL values succeed. The conversion does not mutate the
input before success, so input/output aliasing is safe and a failed
instruction does not publish a partial output.

This instruction is intentionally not a general assertion or dynamic-type
opcode. Its only initial compiler use is LIMIT and OFFSET.

#### REAL-affinity read instruction

Add:

```cpp
struct RealAffinityInstruction {
  RegisterId input;
  RegisterId output;
};
```

The verifier validates both registers, requires the input initialized, and
marks the output initialized.

The VM:

- converts INTEGER to REAL with the same binary64 conversion already used by
  SQLite value semantics;
- leaves NULL, REAL, TEXT, and BLOB unchanged; and
- commits the output only after any required clone succeeds.

Input/output aliasing is valid. An in-place non-INTEGER operation is a no-op.
This instruction deliberately differs from general REAL affinity, which may
parse numeric TEXT.

Lowering emits `RealAffinityInstruction` immediately after
`ReadFieldInstruction` for a bound source-column occurrence whose declared
affinity is REAL. It is not emitted for rowid expressions or INTEGER PRIMARY
KEY aliases.

### Program metadata

Lowering copies the following immutable execution metadata:

- `CatalogVersion::schema_cookie` and `generation` become
  `SchemaVersionRequirement`;
- `BoundSelect::parameters().size()` becomes the program parameter count;
- each bound result column becomes `ResultColumnMetadata` with the same name,
  declared type, and affinity;
- bound literal values become bytecode constants;
- materialized missing-record column defaults become bytecode constants;
- bound function and collation names become bytecode symbols; and
- physical source metadata becomes at most one read-cursor descriptor.

The program does not retain:

- `PhysicalPlan`, `LogicalPlan`, `BoundSelect`, or catalog IDs;
- syntax nodes or SQL source spans;
- function or collation pointers;
- table, column, or index names not needed as runtime symbols; or
- mutable lowering state.

The VM continues to resolve symbol names against the execution environment
once during `ReadVm::Create()`.

Parameter names and `BoundSelect::registration_generation()` remain in the
retained physical plan rather than executable bytecode. The next prepared
statement must retain that plan, or copy equivalent session metadata, so it
can:

- implement named-parameter binding; and
- invalidate or reprepare when function/collation registration generation
  changes.

Registration generation is required for correctness because the optimizer may
have relied on the selected function's determinism or a selected collation's
semantics. It is session metadata, not an instruction operand.

### Cursor descriptor lowering

The selected physical access leaf determines whether a cursor exists.
`PhysicalSingleRowNode` and `PhysicalEmptyNode` use no cursor.
`PhysicalTableScanNode` and `PhysicalRowIdLookupNode` use cursor zero.

ADR-0036 extends record-backed field metadata:

```cpp
enum class MissingFieldValueKind : std::uint8_t {
  kNull,
  kConstant,
  kUnsupported,
};

struct CursorFieldSource {
  CursorFieldSourceKind kind;
  std::uint32_t record_field;
  MissingFieldValueKind missing_value_kind =
      MissingFieldValueKind::kNull;
  std::optional<ConstantId> missing_value{};
};
```

For a record field:

- `kNull` has no constant and returns NULL when the physical record is too
  short;
- `kConstant` references one program constant and clones it only when the
  physical field is absent; and
- `kUnsupported` has no constant and reports `ErrorCode::kGeneric` with an
  explicit unsupported-default diagnostic only if the physical field is
  absent.

A stored NULL is not a missing field and never receives the default.
Rowid-backed fields require `kNull` with no constant because the missing-field
policy is inapplicable.

The bytecode verifier validates the enum, constant reference, and legal
kind/constant combinations. The VM compares the requested physical index with
the current record's actual field count before reading its decoded slot.

Catalog column metadata gains:

```cpp
std::shared_ptr<const SqlValue> missing_record_value;
```

An absent default expression means a missing field yields NULL. When a
default expression exists:

- a non-null `missing_record_value`, including one that points to SQL NULL, is
  the affinity-applied immutable value for an older short record; and
- a null `missing_record_value` marks a legacy or otherwise unsupported
  default that cannot be materialized safely.

The immutable shared pointer keeps aggregate catalog inputs copyable while
`SqlValue` remains move-only. Lowering clones the value into the independent
bytecode constant pool.

The catalog loader materializes every SQLite ALTER-compatible literal default
while loading the schema, applies the column affinity, and stores the value.
This includes NULL, signed numeric, text, blob, and unquoted TRUE/FALSE
literal defaults. Quoted identifiers never materialize as booleans. Catalog
model callers that provide a default expression without a materialized value
remain valid, but lowering emits `kUnsupported` so a complete record can
still execute and a genuinely short record fails explicitly rather than
returning a success-shaped NULL.

#### Ordinary rowid tables

An ordinary table becomes:

```text
storage = kRowIdTable
root_page = CatalogTable::root_page
record_field_count = CatalogTable::columns.size()
```

The descriptor exposes one field per bound source column:

- the INTEGER PRIMARY KEY alias maps to `CursorFieldSourceKind::kRowId`; and
- every other column maps to the record field with the same declaration
  position.

Direct `BoundRowIdExpression` occurrences lower to
`ReadRowIdInstruction`. Bound column occurrences, including an INTEGER
PRIMARY KEY alias, lower to `ReadFieldInstruction` through the descriptor.
Every record-backed column also receives its catalog-derived missing-field
policy.

#### `sqlite_schema`

The schema table is a rowid table rooted at page 1 with five physical fields:

1. `type`;
2. `name`;
3. `tbl_name`;
4. `rootpage`; and
5. `sql`.

Its bound source columns map to record fields zero through four. Direct rowid
expressions use `ReadRowIdInstruction`.

#### WITHOUT ROWID tables

A WITHOUT ROWID table becomes an index-backed cursor rooted at the table root
page. Lowering obtains the validated primary-key index from the retained
catalog.

The primary index's complete `terms` vector is the physical record layout:

- unique primary-key terms appear first in declared key order;
- remaining table columns follow in declaration order; and
- every term retains its effective collation and sort order.

`record_field_count` and `index_columns.size()` equal the complete term count.
Each logical source column maps to the physical term containing its
`ColumnId`. Every term must be a column term; an absent primary index,
expression term, rowid term, duplicate mapping, or missing column is an
internal invariant failure.

The descriptor copies each term's collation symbol and maps ascending or
descending catalog order to `BytecodeSortOrder`.
Each logical column's missing-field policy follows its catalog column even
though its physical record index may differ from declaration order.

This node still performs only a full scan of a WITHOUT ROWID primary B-tree.
Primary-key lookup optimization remains deferred.

### Deterministic register layout

Lowering computes the complete register count before creating the
`ProgramBuilder`. Registers are assigned in deterministic groups:

1. one home register for every `BoundExpressionId`;
2. one exact contiguous argument block for every bound scalar-call
   expression, in bound-expression order;
3. one contiguous result block in result-column order; and
4. only the control registers required by the selected access and LIMIT
   shape.

The optional control registers are:

- a LIMIT counter;
- an OFFSET counter;
- integer zero;
- integer one;
- one comparison temporary; and
- one negative-LIMIT flag for a repeatable table scan.

A zero-argument scalar call reserves no argument registers and uses an empty
range. All count arithmetic is checked before narrowing to the 32-bit
bytecode identities. The configured `ProgramLimits` remain authoritative.

Expression home registers are reused across rows and across repeated
occurrences, but instructions are re-emitted for every occurrence. Result
roots and scalar arguments are compiled directly into their destination
range when possible, avoiding runtime `CopyInstruction` operations and
TEXT/BLOB cloning. The initial lowering does not perform liveness-based
register reuse or SSA construction.

### Constant and symbol assignment

Each bound literal expression receives one bytecode constant in bound arena
order. Repeated emission of that bound occurrence reuses its constant ID.
Lowering adds shared internal NULL, zero, or one constants only when a
control-flow template needs them.

Bound collation and scalar-function IDs map to copied symbol IDs. Additional
symbols are added for:

- the `BINARY` collation used by internal numeric comparisons when no bound
  BINARY symbol already exists; and
- WITHOUT ROWID physical-index collations not already represented by a bound
  collation.

Symbol identity is an assembly detail; duplicate equal symbol strings are
legal. Instructions and cursor metadata always reference the exact emitted
symbol ID.

### Scalar expression lowering

Expression emission is recursive, left-to-right, and destination-directed.
It returns the register containing the occurrence result.

| Bound expression | Bytecode |
|---|---|
| literal | `LoadConstantInstruction` |
| parameter | `LoadParameterInstruction` |
| source column | `ReadFieldInstruction`, followed by `RealAffinityInstruction` for REAL affinity |
| rowid | `ReadRowIdInstruction` |
| unary plus | emit the operand directly into the destination |
| unary negate, bitwise NOT, logical NOT | emit operand, then `UnaryInstruction` |
| arithmetic, concatenation, bitwise | emit left then right, then `BinaryInstruction` |
| scalar AND/OR | apply the narrow bound literal-truth hint; otherwise emit left then right and `BinaryInstruction` |
| comparison | emit left then right, then `CompareInstruction` with bound affinity and collation |
| scalar call | emit arguments left-to-right into its contiguous block, then `CallScalarInstruction` |
| alias reference | emit the target occurrence into the requested destination |
| COLLATE | emit the operand; comparison/function metadata already carries the selected collation |
| likelihood | emit the operand; probability is an optimizer hint only |
| truth test | branch on SQL truth or NULL and materialize integer zero or one |
| coalesce / ifnull | lazy left-to-right NULL branches |
| iif / if | lazy condition/value pairs with an optional default |

#### Scalar AND/OR

`BoundBinaryOperation::kLogicalAnd` and `kLogicalOr` are scalar value
operations. Normally both operands are emitted left-to-right before
`BinaryInstruction`, preserving the existing three-valued VM operation.

Pinned SQLite performs one narrow dead-side simplification before emission.
The binder therefore records an expression truth hint:

```cpp
enum class BoundTruthHint : std::uint8_t {
  kNone,
  kAlwaysFalse,
  kAlwaysTrue,
};
```

The hint is set only for:

- unresolved unquoted TRUE/FALSE; or
- a direct, underscore-free decimal or hexadecimal INTEGER token accepted by
  SQLite's signed-32-bit literal test, with zero false and nonzero true; or
- SQLite's parser-folded non-NULL literal `IS NULL` / `IS NOT NULL` forms,
  including the permitted unary numeric wrappers.

Parentheses remain transparent because binding removes them. Alias references
copy the target hint because SQLite alias substitution retains the copied
expression flags. COLLATE clears the hint. Other unary operations,
likelihood, standalone REAL/NULL/TEXT/BLOB literals, underscored integers, and
integer values greater than `INT32_MAX` are barriers or have no hint.

The ordered simplification rules are:

| Condition, in order | AND emits | OR emits |
|---|---|---|
| left always true or right always false | right only | left only |
| otherwise, right always true or left always false | left only | right only |

The selected occurrence is still converted to SQL truth. Lowering emits it
once, then applies the same logical binary operation with that register as
both operands. The result is 0, 1, or NULL rather than the selected operand's
raw value.

Examples:

- `0 AND failing()` skips `failing()` and returns 0;
- `2 OR failing()` skips `failing()` and returns 1;
- `0.0 AND failing()` evaluates `failing()`; and
- `+0 AND failing()` evaluates `failing()`.

WHERE conjunction short-circuiting is represented separately by the physical
Guard and Filter predicate vectors.

An alias reference never reads a previously projected register. It emits its
target again at the alias occurrence, preserving non-deterministic and
side-effecting occurrence semantics.

#### Truth tests

For `IS TRUE`, `IS FALSE`, and `IS NULL`, lowering:

1. emits the operand once;
2. initializes the destination to the nonmatching integer result;
3. branches on true, false, or NULL as appropriate;
4. overwrites the destination with the matching integer result; and
5. joins with the destination definitely initialized.

`IS NOT` swaps the matching and nonmatching results. Truth tests never
produce NULL.

#### Coalesce

For every argument except the final argument, lowering emits that argument
directly into the destination and jumps to the expression end when the value
is not NULL. The next argument overwrites the same destination only on the
NULL path. The final argument is always emitted on the final path.

No unselected argument executes.

#### Conditional expressions

`iif` and `if` arguments are interpreted as condition/value pairs followed
by an optional default. Conditions execute left-to-right. False and NULL
conditions continue to the next pair. A true condition emits only its paired
value and jumps to the expression end.

An odd argument count uses the final argument as the default. An even
argument count materializes NULL when no condition is true. No unselected
value or default executes.

### LIMIT and OFFSET initialization

When a physical Limit node exists, lowering emits:

1. the LIMIT expression into the LIMIT counter;
2. `MustBeIntegerInstruction` in place;
3. a false jump from integer zero to statement completion;
4. the OFFSET expression, when present, into the OFFSET counter;
5. `MustBeIntegerInstruction` in place; and
6. a one-time comparison that replaces a negative OFFSET with zero.

No OFFSET instruction is reachable from the LIMIT-zero edge. A LIMIT or
OFFSET type mismatch ends execution with `kTypeMismatch` before guards,
source initialization, filtering, or projection.

For a repeatable table scan, lowering also computes once whether LIMIT is
negative. After each resumed result row:

- a negative LIMIT continues without decrement;
- a positive LIMIT decrements by one; and
- a counter reaching zero reaches the positioned-cursor halt.

SingleRow and RowIdLookup accesses can produce at most one row, so they need
no post-result LIMIT decrement.

OFFSET applies only after all residual predicates pass. A positive OFFSET is
decremented and the row is skipped before any projection expression runs.

### Guard and filter lowering

Each physical Guard predicate is emitted once in stored order before key
evaluation or cursor opening. False or NULL branches to statement
completion. True continues to the next guard.

For a table source, Guard contains deterministic source-independent
predicates. For `PhysicalSingleRowNode`, ADR-0035 is amended so every unknown
WHERE conjunct is a Guard predicate regardless of determinism. This preserves
SQLite's no-FROM left-to-right predicate order and ensures that a
non-deterministic conjunct preceding a known-false conjunct still executes.

Each physical Filter predicate is emitted once in stored order for every
candidate row. False or NULL rejects the row immediately:

- a table scan advances to the next row;
- a rowid lookup reaches its positioned-cursor halt; and
- a single-row source completes.

The selected rowid equality is absent from the Filter by ADR-0035 and is not
evaluated a second time.

### Access-path control flow

#### SingleRow

After LIMIT/OFFSET initialization and guards, the program evaluates optional
filters for one synthetic row, applies OFFSET, evaluates projection
expressions in result order, emits one `ResultRowInstruction`, and halts when
execution resumes.

#### Empty

After LIMIT/OFFSET initialization and preserved guards, the program halts.
It emits no source, filter, offset-row, projection, or result instructions.

#### TableScan

Lowering:

1. opens cursor zero;
2. rewinds it, branching on empty;
3. binds a positioned row-loop label;
4. evaluates filters;
5. applies OFFSET;
6. evaluates and emits one result row;
7. applies positive LIMIT termination after resume; and
8. advances with `NextInstruction`.

The rewind-empty and next-exhausted edges reach one halt with an unpositioned
cursor. A LIMIT-complete edge reaches a separate halt with a positioned
cursor. Pre-open completion uses a third closed-cursor halt when required.
Those targets remain separate because the bytecode verifier rejects different
cursor states at a join. `HaltInstruction` performs the actual cursor cleanup.

#### RowIdLookup

Lowering opens cursor zero, evaluates the retained key once, and emits
`SeekRowIdInstruction`. Opening first preserves SQLite's storage-open error
precedence over a key-expression error.

The missing or unconvertible-key edge reaches an unpositioned-cursor halt.
The hit edge evaluates residual predicates, applies OFFSET, projects at most
one row, then reaches a separate positioned-cursor halt after result resume.
Pre-open guard or LIMIT completion reaches a closed-cursor halt. The states do
not join before halt.

### Projection and result suspension

Lowering reads the retained logical projection referenced by the physical
Projection node. It emits each result expression occurrence from left to
right directly into the contiguous result block, then emits:

```cpp
ResultRowInstruction{
    .first = result_block_first,
    .count = result_column_count,
};
```

The VM exposes that range and resumes at the instruction after the result
row. OFFSET rows never execute projection. Repeated result expressions and
alias targets are emitted per occurrence rather than memoized.

### Builder and verifier boundary

All branches use `ProgramBuilder` labels. Lowering does not construct final
instruction addresses or patch published operations.

The builder receives:

- the exact schema-version requirement;
- the complete register and parameter counts; and
- caller-supplied `ProgramLimits`.

`ProgramBuilder::Build()` resolves labels and runs the bytecode verifier.
`LowerReadPlan()` publishes a program only after:

- every resource reference is in range;
- every result range matches result metadata;
- all registers are definitely initialized;
- every cursor transition is valid;
- all branch targets are valid;
- every instruction is reachable; and
- no path falls through the program end.

Lowering does not add a second plan executor or bypass verification for
compiler-produced programs.

Simple SELECT programs need no explicit `CloseCursorInstruction`. The VM
already closes every cursor on halt, error, reset, and destruction, matching
SQLite's `sqlite3VdbeHalt()` cleanup. Multiple halt instructions preserve the
verifier's exact cursor-state joins without adding successful-path close work.

### Layering

The public lowering header may include only:

- standard-library headers;
- `modern_sqlite/base/result.hpp`;
- `modern_sqlite/bytecode/program.hpp`; and
- `modern_sqlite/optimizer/physical_plan.hpp`.

The implementation may depend on lowering, bytecode, optimizer, logical-plan,
binder, catalog, and SQL-value contracts. It must not include:

- VM;
- pager, B-tree, cache, VFS, record codec, or database-format internals;
- session or public API types;
- diagnostics; or
- parser implementation.

The optimizer remains unaware of bytecode. The bytecode and VM remain unaware
of catalog, bound-expression, logical-plan, physical-plan, and lowering
types.

### Complexity, allocation, and performance

Register planning, pool construction, cursor-descriptor construction, and
instruction emission are linear in retained metadata plus emitted expression
occurrences. Alias and projection occurrences may intentionally re-emit a
shared bound target because memoization would change observable evaluation.

The implementation:

- performs no SQL parsing, binding, catalog name lookup, or access-path
  selection;
- copies only execution-owned values and metadata;
- uses one cursor at most;
- uses deterministic register and label assignment;
- emits scalar arguments and result expressions directly into contiguous
  destination ranges;
- emits no explicit close instruction for simple read completion;
- performs no heap allocation while traversing a published program; and
- relies on the existing compact instruction variant and verifier.

OOM tests inject every allocation boundary for a representative program
containing constants, symbols, a cursor descriptor, lazy expressions,
guards, filters, LIMIT/OFFSET, and result metadata. Allocation tests verify
that published-program traversal remains allocation-free. The reviewed
implementation performs 17 allocations for a constant row, 27 for a table
scan, 26 for a direct rowid lookup, and 33 for a LIMIT/OFFSET scan. These
counts include bytecode verification and are locked by dedicated tests.

The Release performance benchmark compares:

- Modern SQLite parse + bind + logical plan + optimize + lower; and
- pinned SQLite 3.54.0 `sqlite3_prepare_v3()` for the same executable SELECT.

Both sides use a preloaded equivalent schema, warm process state, the same
compiler optimization level, and constant-row, full-scan, direct-rowid,
residual-rowid, and LIMIT/OFFSET cases. Database open, schema loading, result
stepping, reset, destruction, and explain formatting are outside the timed
region.

The initial severe-regression guard requires every Modern median to remain
within 10 times the pinned SQLite median. The report includes provenance,
raw and median timings, ratios, emitted instruction/register counts, verifier
metrics, and allocation counts.

The reviewed implementation's final Apple Clang 21 arm64 benchmark produced:

| Workload | Modern median | SQLite median | Ratio | Modern allocations | SQLite allocations | Instructions | Registers |
|---|---:|---:|---:|---:|---:|---:|---:|
| Constant row | 2171.33 ns | 984.95 ns | 2.204518 | 36 | 18 | 4 | 4 |
| Full scan | 1956.91 ns | 854.80 ns | 2.289335 | 55 | 21 | 8 | 4 |
| Direct rowid | 1794.38 ns | 747.33 ns | 2.401050 | 58 | 19 | 7 | 5 |
| Residual rowid | 2392.71 ns | 1068.28 ns | 2.239792 | 65 | 26 | 12 | 9 |
| LIMIT/OFFSET | 3177.51 ns | 1239.88 ns | 2.562763 | 75 | 31 | 33 | 15 |

The maximum ratio is 2.562763, so every workload passes the 10x
severe-regression gate. Raw samples, verifier metrics, source hashes, and
provenance are stored in the session artifacts
`read_lowering_benchmark.cpp`, `read_lowering_benchmark-results.json`, and
`read_lowering_benchmark-provenance.json`.

## Verification

Development follows ADR-0005:

1. accept this ADR after independent design review;
2. add the bytecode extension and public lowering contract with red-first
   tests;
3. confirm the expected missing-lowering-symbol failure;
4. implement the smallest complete lowering;
5. build and validate the pinned performance benchmark;
6. run focused and complete validation; and
7. obtain independent final code review before commit.

Tests cover:

- stable lowering error names and base-code mappings;
- invalid and moved-from physical-plan rejection;
- schema version, parameter count, result metadata, constants, and symbols;
- verified exact program shapes for constant-row, Empty, table-scan, and
  rowid access;
- ordinary rowid-table, INTEGER PRIMARY KEY alias, `sqlite_schema`, and
  WITHOUT ROWID cursor descriptors;
- source-field and physical-field mappings;
- short-record default substitution, stored-NULL preservation, affinity,
  unsupported-default failure, and WITHOUT ROWID reordered fields;
- literal, parameter, column, rowid, unary, binary, comparison, truth-test,
  scalar-call, alias, COLLATE, and likelihood lowering;
- integer-only REAL-affinity conversion after REAL source-column reads;
- SQLite's narrow scalar AND/OR dead-side simplification and truthification;
- contiguous scalar arguments and result ranges;
- lazy coalesce, ifnull, iif, and if evaluation;
- scalar AND/OR evaluation and physical conjunction short-circuiting;
- LIMIT-before-OFFSET order, strict conversion, zero bypass, negative LIMIT,
  negative OFFSET, filtered-row counting, projection suppression, and
  post-result termination;
- one-time statement guards and rowid keys;
- residual predicate order and consumed rowid-term removal;
- separate closed, positioned, and unpositioned halt paths;
- direct bytecode tests for `MustBeIntegerInstruction` and
  `RealAffinityInstruction`, including aliased registers and pinned
  differential values;
- end-to-end parse/bind/plan/optimize/lower/verify/execute results against
  pinned fixtures;
- nested program-limit propagation;
- allocation failure at every lowering boundary;
- allocation-free published-program access;
- the compiler performance baseline and 10x guard;
- lowering, bytecode, optimizer, and VM dependency-layer enforcement;
- Debug and Release builds;
- ASan/UBSan and TSan;
- clang-tidy and warnings-as-errors on GCC and Clang;
- formatting, graph, layering, and whitespace checks; and
- independent final review.

## Explicitly deferred behavior

- Ordinary-index, covering-index, and WITHOUT ROWID key seeks.
- Rowid ranges, IN, OR unions, joins, sorting, DISTINCT, aggregation,
  grouping, compounds, windows, subqueries, and coroutines.
- Writes, transactions, journals, triggers, virtual tables, and schema
  mutation.
- Common-subexpression elimination or alias-result memoization.
- General `OP_Once`-style factoring of deterministic source-independent
  projection or lazy-branch expressions. Statement guards still execute once,
  but callback invocation counts for custom functions declared deterministic
  are not a compatibility guarantee in this milestone.
- Liveness-based register reuse and spill management.
- Bytecode serialization or a stable external opcode format.
- Prepared-statement ownership, transaction orchestration, and public
  bind/step/reset APIs.

## Consequences

- The read compiler now has an explicit, independently testable boundary from
  physical plan to verified immutable bytecode.
- The VM remains independent of every compiler and catalog representation.
- LIMIT and OFFSET gain SQLite-compatible strict runtime conversion without
  broadening cast or affinity semantics.
- REAL-affinity columns preserve SQLite's integer-only read conversion,
  including files whose stored TEXT does not match the current declaration.
- Older short records receive their affinity-applied declared default without
  conflating a stored NULL with a missing field.
- Evaluation order, lazy special forms, alias occurrences, guards, rowid
  keys, OFFSET, projection, and result suspension are explicit in generated
  control flow.
- Ordinary and WITHOUT ROWID table layouts become bytecode-owned cursor
  descriptors with no runtime catalog dependency.
- The first register allocation is intentionally simple and deterministic;
  later performance work can add reuse without changing the bytecode or SQL
  semantics.

## Alternatives considered

### Execute physical nodes directly

Rejected. It would couple execution to optimizer and catalog ownership,
bypass bytecode verification, and violate the dependency DAG.

### Use `ApplyAffinityInstruction` for LIMIT and OFFSET

Rejected. Numeric affinity leaves invalid values unchanged, so lowering could
not produce SQLite's required `SQLITE_MISMATCH`.

### Use `CastInstruction` for LIMIT and OFFSET

Rejected. Forced integer casts accept and truncate values that SQLite rejects
for LIMIT and OFFSET.

### Use general REAL affinity after a source-column read

Rejected. `ApplyAffinity(kReal)` also parses numeric TEXT, while SQLite's
`OP_RealAffinity` converts only INTEGER storage.

### Add dedicated LIMIT and OFFSET opcodes

Rejected. Existing constants, arithmetic, comparisons, labels, and jumps
already express counter control. Only strict integer conversion is missing.

### Cache every bound expression by ID

Rejected. Bound IDs identify retained nodes, not one-time values. Alias and
duplicate result occurrences may contain non-deterministic or side-effecting
functions and must execute per occurrence.

### Emit final numeric jump addresses directly

Rejected. `ProgramBuilder` labels preserve ownership, reject unresolved or
foreign targets, and guarantee one verification path for hand-authored and
compiler-produced programs.

### Treat WITHOUT ROWID tables as rowid tables

Rejected. Their table storage is the primary index B-tree, their physical
field order differs from declaration order, and rowid operations are invalid.

### Return NULL for every missing physical field

Rejected. SQLite substitutes an ALTER-added column's declared default only
when the record is physically short. Returning NULL silently changes both
value and type, while substituting on a stored NULL would also be incorrect.

### Emit explicit close instructions on every successful path

Rejected. The VM already owns cleanup on halt and failure. Separate halt
targets preserve verifier cursor states with fewer instructions and match
SQLite's simple SELECT programs.

### Join positioned and unpositioned cursor paths before halt

Rejected. The verified cursor-state lattice deliberately rejects that join.
Separate halt instructions preserve both states until VM-owned cleanup.
