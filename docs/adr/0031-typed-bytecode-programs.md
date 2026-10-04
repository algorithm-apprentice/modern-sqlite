# ADR-0031: Typed Immutable Bytecode Programs

- Status: Accepted
- Date: 2026-10-04

## Context

The read VM, lowering layer, and prepared-statement session need a common
executable representation. The representation must be compact enough for a
dispatch loop, expressive enough for the first read-only milestone, and
strict enough that the VM can execute trusted instructions without repeating
compiler validation on every step.

Pinned SQLite 3.54.0 provides the behavioral reference but not an internal
compatibility target:

- `src/vdbe.h:50-93` stores every instruction in a generic `VdbeOp` with an
  opcode, three integer operands, an untyped ownership-sensitive `p4` union,
  and a `p5` flag word;
- `src/vdbeaux.c:271-363` appends mutable operations and grows the operation
  array during code generation;
- `src/vdbeaux.c:598-658` represents unresolved labels as negative integer
  addresses held by the parser;
- `src/vdbeaux.c:876-975` resolves jump operands in place and derives selected
  program properties immediately before publication;
- `src/vdbeaux.c:1286-1355` permits arbitrary post-emission opcode and operand
  patching;
- `src/vdbeaux.c:2658-2755` converts the mutable VDBE into ready state and
  allocates registers, parameters, and cursor slots from parser-owned counts;
- `src/vdbeInt.h:458-488` places instructions, registers, parameters, cursors,
  result state, and execution state in one mutable connection-owned object;
- `src/vdbeaux.c:2804-2860` closes all remaining cursors when the VM is reset
  or deleted;
- `src/vdbeaux.c:2871-2890` records result-column metadata before execution;
  and
- `src/sqliteLimit.h:72-75`, `src/sqliteLimit.h:136-138`, and
  `src/sqliteLimit.h:189-190` define default limits of 2,000 result columns,
  250,000,000 VDBE operations, and 32,766 bound variables.

The pinned arm64 build uses 24-byte, 8-byte-aligned `VdbeOp` objects. That is
a useful footprint reference, but SQLite explicitly documents VDBE opcodes as
an unstable internal interface. Modern SQLite therefore preserves observable
SQL and database behavior rather than opcode numbers, operand positions, or
the `P4` ownership model.

The architecture requires stronger dependency boundaries than SQLite:

- the parser produces immutable syntax and cannot emit bytecode;
- the binder and optimizer cannot mutate VM addresses;
- only lowering emits bytecode;
- the VM depends on bytecode but not on syntax, binding, or plan types;
- the B-tree never depends on VM registers; and
- every program is immutable before execution begins.

The next node executes hand-authored read programs. This node must therefore
define enough instructions for constants, parameters, table and index
cursors, rowid access, column reads, comparisons, scalar functions, control
flow, result suspension, and clean termination without implementing the VM
itself.

## Decision

Add typed bytecode contracts under
`modern_sqlite/bytecode/program.hpp` with implementation in
`src/bytecode/program.cpp`.

### Strong identities

All executable resource references are distinct unsigned 32-bit value types:

```cpp
struct InstructionAddress {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const InstructionAddress&) const noexcept = default;
};

struct RegisterId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const RegisterId&) const noexcept = default;
};

struct CursorId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const CursorId&) const noexcept = default;
};

struct ParameterId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const ParameterId&) const noexcept = default;
};

struct ConstantId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const ConstantId&) const noexcept = default;
};

struct SymbolId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const SymbolId&) const noexcept = default;
};

struct RootPageNumber {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const RootPageNumber&) const noexcept = default;
};

struct CursorFieldId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const CursorFieldId&) const noexcept = default;
};
```

`InstructionAddress` exists only in finalized instructions.
`RootPageNumber` is bytecode-owned execution metadata and does not expose the
pager's `PageNumber` type across the dependency boundary.

Labels use a separate opaque class whose constructor is private to
`ProgramBuilder`. Each label contains a builder identity and dense local
index. No signed integer or sentinel value can be interpreted as both an
address and a label, and labels from different builders cannot alias.

Contiguous register arguments and result rows use:

```cpp
struct RegisterRange {
  RegisterId first{};
  std::uint32_t count = 0;
};
```

Range validation uses checked arithmetic. Empty ranges are accepted only
where the instruction contract explicitly allows them, such as a zero-arity
scalar function.

### Bytecode-owned cursor descriptors

The bytecode layer does not include the catalog or syntax headers. Lowering
converts catalog objects into execution-only descriptors:

```cpp
enum class CursorStorageKind : std::uint8_t {
  kRowIdTable,
  kIndex,
};

enum class CursorFieldSourceKind : std::uint8_t {
  kRecordField,
  kRowId,
};

struct CursorFieldSource {
  CursorFieldSourceKind kind = CursorFieldSourceKind::kRecordField;
  std::uint32_t record_field = 0;
};

enum class BytecodeSortOrder : std::uint8_t {
  kAscending,
  kDescending,
};

struct IndexColumnMetadata {
  SymbolId collation{};
  BytecodeSortOrder order = BytecodeSortOrder::kAscending;
};

struct ReadCursorDescriptor {
  RootPageNumber root_page{};
  CursorStorageKind storage = CursorStorageKind::kRowIdTable;
  std::uint32_t record_field_count = 0;
  std::vector<CursorFieldSource> fields{};
  std::vector<IndexColumnMetadata> index_columns{};
};
```

Each `CursorId` indexes exactly one descriptor. A rowid table descriptor uses
the table B-tree. An index descriptor uses the index B-tree for an explicit
index or a WITHOUT ROWID table and carries comparison metadata for every
physical record field.

`fields` is the lowering-defined projection from executable field IDs to
physical record fields or the rowid. For a rowid table it can map an INTEGER
PRIMARY KEY alias to `kRowId`; for a WITHOUT ROWID table it maps logical
columns to their reordered physical fields; for an index it exposes physical
key and suffix fields. `ReadFieldInstruction` therefore does not need table,
index, column, AST, or catalog types.

The later prepared statement retains the immutable catalog snapshot
separately from the bytecode program. Lowering copies only the schema version
requirement and physical execution metadata into bytecode. TEMP and attached
schemas remain session policy.

### Symbols and constants

The program owns two immutable pools:

- `std::vector<SqlValue>` for NULL, integer, real, text, and blob constants;
- `std::vector<std::string>` for collation and scalar-function names.

Instructions reference these pools through `ConstantId` and `SymbolId`.
This keeps executable instructions fixed-size and avoids repeated owned
strings in the dispatch stream.

Symbol lookup is deliberately deferred to the VM's connection-owned
registries. The binder and lowering layers are responsible for emitting
resolved names, while the VM still reports a missing runtime registration
explicitly rather than substituting a fallback.

The VM resolves function symbols with their argument counts and resolves
collation symbols once when initializing execution. The dispatch loop uses
the resolved handles and does not repeat string lookup for every comparison
or function invocation. A scalar call always carries a selected collation;
functions that do not inspect it receive the explicit `BINARY` symbol.

### Initial instruction set

Each opcode is represented by its own aggregate payload:

```cpp
struct HaltInstruction {};

struct LoadConstantInstruction {
  ConstantId constant{};
  RegisterId output{};
};

struct LoadParameterInstruction {
  ParameterId parameter{};
  RegisterId output{};
};

struct CopyInstruction {
  RegisterId input{};
  RegisterId output{};
};

enum class UnaryOperation : std::uint8_t {
  kNegate,
  kBitwiseNot,
  kNot,
};

struct UnaryInstruction {
  UnaryOperation operation = UnaryOperation::kNegate;
  RegisterId input{};
  RegisterId output{};
};

enum class BinaryOperation : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kRemainder,
  kConcatenate,
  kBitwiseAnd,
  kBitwiseOr,
  kShiftLeft,
  kShiftRight,
  kAnd,
  kOr,
};

struct BinaryInstruction {
  BinaryOperation operation = BinaryOperation::kAdd;
  RegisterId left{};
  RegisterId right{};
  RegisterId output{};
};

struct ApplyAffinityInstruction {
  RegisterId input{};
  RegisterId output{};
  TypeAffinity affinity = TypeAffinity::kNone;
};

struct CastInstruction {
  RegisterId input{};
  RegisterId output{};
  CastTarget target = CastTarget::kNumeric;
};

struct OpenReadInstruction {
  CursorId cursor{};
};

struct CloseInstruction {
  CursorId cursor{};
};

struct RewindInstruction {
  CursorId cursor{};
  InstructionAddress if_empty{};
};

struct NextInstruction {
  CursorId cursor{};
  InstructionAddress if_next{};
};

struct SeekRowIdInstruction {
  CursorId cursor{};
  RegisterId rowid{};
  InstructionAddress if_missing{};
};

struct ReadFieldInstruction {
  CursorId cursor{};
  CursorFieldId field{};
  RegisterId output{};
};

struct ReadRowIdInstruction {
  CursorId cursor{};
  RegisterId output{};
};

struct CompareInstruction {
  RegisterId left{};
  RegisterId right{};
  RegisterId output{};
  SqlComparison comparison = SqlComparison::kEqual;
  TypeAffinity affinity = TypeAffinity::kNone;
  SymbolId collation{};
};

struct CallScalarInstruction {
  SymbolId function{};
  SymbolId collation{};
  RegisterRange arguments{};
  RegisterId output{};
};

struct JumpInstruction {
  InstructionAddress target{};
};

enum class JumpCondition : std::uint8_t {
  kTrue,
  kNotTrue,
  kFalse,
  kNotFalse,
  kNull,
  kNotNull,
};

struct JumpIfInstruction {
  RegisterId condition{};
  JumpCondition test = JumpCondition::kTrue;
  InstructionAddress target{};
};

struct ResultRowInstruction {
  RegisterRange values{};
};
```

`Instruction` is a `std::variant` of these payloads. `InstructionKind`,
`InstructionKindOf()`, and `InstructionKindName()` provide stable diagnostic
names without assigning SQLite-compatible opcode numbers.

The instruction set is intentionally smaller than SQLite's VDBE:

- `CompareInstruction` produces a SQL truth value in a register, and
  `JumpIfInstruction` performs control flow separately. The comparison
  carries the affinity and collation selected by binding;
- unary, binary, affinity, and cast instructions cover the initial executable
  expression matrix without importing syntax enums;
- `OpenReadInstruction` references a bytecode-owned cursor descriptor rather
  than a catalog object, database slot, `KeyInfo*`, or ownership-tagged union;
- `ReadFieldInstruction` uses the descriptor's lowering-defined physical
  mapping;
- `CallScalarInstruction` uses a contiguous argument range and a symbol-pool
  reference rather than a raw function pointer, and carries the selected
  collation explicitly;
- `ResultRowInstruction` suspends execution after exposing an immutable view
  of one register range. That view remains valid until the next step, reset,
  or finalization; and
- `HaltInstruction` represents successful completion only. Error and write
  termination operands are deferred until their execution semantics exist.

The initial executable expression matrix includes literals, parameters,
cursor fields, rowids, unary numeric/bitwise/NOT operations, arithmetic,
bitwise operations, concatenation, SQL AND/OR, affinity, CAST, comparisons,
COLLATE, and registered scalar functions. Registers are mutable, so
`LIMIT`/`OFFSET` lower to integer constants, arithmetic, comparison, and
branches without a dedicated counter opcode.

Pattern operators, aggregates, sorting, joins, subprograms, coroutines,
writes, transactions, virtual tables, triggers, and schema mutation remain
outside the read MVP. Binder diagnostics reject unsupported executable
expressions until their runtime semantics are added. New typed instruction
alternatives are added with the corresponding VM behavior and verifier rules,
not merely because the parser recognizes more syntax.

`SeekRowIdInstruction` applies SQLite's lossless rowid conversion without
mutating its input register: integers are used directly, strings receive
numeric affinity, and a real value is accepted only when it is exactly
representable as a signed 64-bit integer. Conversion failure follows the
missing-target edge. `JumpIfInstruction`, unary `NOT`, and binary `AND`/`OR`
use SQLite numeric truth conversion and preserve three-valued NULL behavior
according to their explicit operation or jump condition.

### Immutable program input and output

Program construction accepts:

```cpp
struct ResultColumnMetadata {
  std::string name{};
  std::optional<std::string> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
};

struct SchemaVersionRequirement {
  std::uint32_t schema_cookie = 0;
  std::uint64_t generation = 0;
};

struct ProgramInput {
  SchemaVersionRequirement schema_version{};
  std::uint32_t register_count = 0;
  std::uint32_t parameter_count = 0;
  std::vector<SqlValue> constants{};
  std::vector<std::string> symbols{};
  std::vector<ReadCursorDescriptor> cursors{};
  std::vector<ResultColumnMetadata> result_columns{};
  std::vector<Instruction> instructions{};
};
```

`BytecodeProgram::Create()` verifies and then moves this storage into a
move-only `BytecodeProgram`. The program exposes only const references,
spans, string views, counts, and checked-by-construction indexed accessors.
No instruction, pool entry, cursor descriptor, resource count, result
metadata, or schema-version requirement can be mutated after publication.

The later prepared statement owns both the program and the exact catalog
snapshot used by lowering. On the first step, the session begins or joins a
read transaction and compares the active pager schema cookie plus the
retained catalog generation with `schema_version` before VM initialization
or cursor opening. That transaction remains active across every
`ResultRowInstruction` suspension. Resume does not revalidate or replace the
catalog under a live VM. Reset or finalization closes VM cursors before the
statement-owned transaction ends; a ready statement may then recompile
against a replacement snapshot.

### Resource limits

`ProgramLimits` defaults to:

```cpp
struct ProgramLimits {
  std::size_t maximum_instructions = 250'000'000;
  std::size_t maximum_registers = 1'000'000;
  std::size_t maximum_cursors = 100'000;
  std::size_t maximum_parameters = 32'766;
  std::size_t maximum_constants = 1'000'000;
  std::size_t maximum_symbols = 1'000'000;
  std::size_t maximum_result_columns = 2'000;
  std::size_t maximum_labels = 10'000'000;
  std::size_t maximum_owned_bytes = 1'000'000'000;
  std::size_t maximum_analysis_words = 16'000'000;
  std::size_t maximum_analysis_edge_words = 250'000'000;
};
```

The instruction, parameter, and result-column defaults preserve SQLite's
published limits. Register, cursor, constant, and symbol defaults are
explicit implementation limits for this MVP and may become connection limits
later. Counts must also fit their 32-bit IDs.

`maximum_owned_bytes` covers instruction storage, SQL-value text/blob
payloads, symbols, result strings, cursor fields, and index metadata using
overflow-safe size calculations. Verification counts retained vector and
string capacities rather than only logical sizes. `BytecodeProgram::Create`
validates a public `ProgramInput`, deep-clones every owning container and
SQL-value payload into canonical storage, and verifies that canonical copy
before publication. Caller-held pointers, references, capacities, and moved
storage therefore cannot mutate or inflate a published program.

`maximum_analysis_words` bounds allocated dataflow state, reachability and
queue bitsets, the worklist, and scratch state.
`maximum_analysis_edge_words` bounds the sum of packed state words processed
across control-flow edges. All ceiling division is overflow-free on 32-bit
and 64-bit targets. `ProgramBuilder` enforces count and logical owned-byte
limits incrementally; direct `ProgramInput` is checked before canonical
publication and verifier-state allocation. A later `std::bad_alloc`
conversion remains the final safety boundary rather than the resource
policy.

Limit failures use `ErrorCode::kTooLarge`. Invalid compiler-produced programs
use `ErrorCode::kMisuse`.

### Stable verification diagnostics

Program construction and assembly return `std::expected` with:

```cpp
enum class ProgramErrorCode : std::uint8_t {
  kEmptyProgram,
  kInstructionLimitExceeded,
  kRegisterLimitExceeded,
  kCursorLimitExceeded,
  kParameterLimitExceeded,
  kConstantLimitExceeded,
  kSymbolLimitExceeded,
  kResultColumnLimitExceeded,
  kLabelLimitExceeded,
  kOwnedBytesLimitExceeded,
  kAnalysisLimitExceeded,
  kAnalysisWorkLimitExceeded,
  kInvalidRootPage,
  kInvalidCursorDescriptor,
  kInvalidRegister,
  kInvalidRegisterRange,
  kInvalidParameter,
  kInvalidConstant,
  kInvalidSymbol,
  kInvalidCursor,
  kInvalidField,
  kInvalidBranchTarget,
  kInvalidEnumValue,
  kInvalidText,
  kResultShapeMismatch,
  kBranchRequiresLabel,
  kInvalidBuilder,
  kForeignLabel,
  kInvalidLabel,
  kLabelAlreadyBound,
  kUnboundLabel,
  kLabelTargetOutOfRange,
  kUninitializedRegister,
  kCursorAlreadyOpen,
  kCursorAlreadyClosed,
  kCursorNotOpen,
  kCursorNotPositioned,
  kCursorStateConflict,
  kRowIdOperationRequiresRowIdTable,
  kFallthroughPastEnd,
  kUnreachableInstruction,
};

struct ProgramError {
  ProgramErrorCode code;
  std::size_t instruction;
  std::uint64_t detail;

  ErrorCode base_error_code() const noexcept;
};

template <typename T>
using ProgramResult = std::expected<T, ProgramError>;
```

Diagnostics identify the failing finalized instruction when one exists.
Assembly-only label failures do not invent an instruction location.

### Structural verification

The verifier first checks every instruction and descriptor, including
instructions later found to be unreachable:

- all declared counts, pool sizes, owned bytes, and projected analysis work
  satisfy `ProgramLimits` and 32-bit ID representation;
- the program is nonempty;
- every register, register range, cursor, parameter, constant, and symbol
  reference is in range;
- every jump target names an existing instruction;
- every cursor descriptor has a nonzero root page; the active pager snapshot
  performs range and reserved-locking-page validation when the VM opens it;
- every exposed cursor field maps to an existing record field, while rowid
  fields occur only on rowid-table descriptors;
- rowid-table descriptors have no index comparison metadata;
- index descriptors provide nonempty comparison metadata for exactly their
  physical record-field count;
- every index comparison entry references a valid collation symbol;
- `SeekRowIdInstruction` and `ReadRowIdInstruction` target a rowid-table
  descriptor, never an index-backed descriptor;
- unary, binary, affinity, cast, comparison, and call instructions reference
  valid operations and symbols;
- every `ResultRowInstruction` exposes exactly the published result-column
  count; and
- an empty result range is valid only when the program publishes zero result
  columns.

Symbol strings and result names must be valid strings for the existing
runtime contracts. Unknown function or collation names are not verifier
errors because connection-owned registration is a VM concern.

### Control-flow and definite-state verification

The verifier then performs a deterministic forward worklist analysis from
instruction zero.

Each program point tracks:

- registers definitely initialized on every incoming path; and
- for every cursor, whether it is closed, open but unpositioned, or open and
  positioned on a valid row.

Instruction transfer rules are:

- constant, parameter, copy, unary, binary, affinity, cast, field, rowid,
  comparison, and scalar-call outputs become initialized;
- copy, unary, binary, affinity, cast, seek, compare, scalar-call,
  conditional-jump, and result-row inputs must already be definitely
  initialized;
- `OpenReadInstruction` requires a closed cursor and transitions it to open
  but unpositioned;
- `RewindInstruction` produces positioned state on its fallthrough edge and
  unpositioned state on its empty-target edge;
- `NextInstruction` produces positioned state on its taken edge and
  unpositioned state on its exhausted fallthrough edge;
- `SeekRowIdInstruction` produces positioned state on its successful
  fallthrough edge and unpositioned state on its missing-target edge;
- field and rowid reads require a definitely positioned cursor;
- other cursor operations require an open cursor of the required storage
  kind;
- `CloseInstruction` requires an open cursor and transitions it to closed;
- opening an already-open cursor, closing a closed cursor, or using a closed
  or unpositioned cursor for row access is rejected;
- register states merge by intersection, so a register is initialized after
  a join only when every predecessor initialized it;
- cursor states must be identical at a join. A cursor that is closed,
  unpositioned, or positioned on different incoming paths is rejected;
- `JumpInstruction` has one target successor;
- `JumpIfInstruction`, `RewindInstruction`, `NextInstruction`, and
  `SeekRowIdInstruction` have both branch and fallthrough successors;
- `HaltInstruction` has no successor;
- `ResultRowInstruction` falls through because stepping resumes at the next
  instruction;
- reachable fallthrough beyond the instruction array is rejected; and
- any instruction not reached by the completed analysis is rejected.

The verifier does not require explicit `CloseInstruction` on every halt path.
The VM owns cursor slots and closes all remaining cursors on halt, error,
reset, and destruction, matching SQLite's VM-level cleanup model. Explicit
close remains useful for releasing resources before statement completion.

This analysis validates compiler output once. The VM may then use assertions
for instruction and resource invariants while still reporting database,
binding, function, collation, and I/O failures through typed results.

### Labels and assembly

`ProgramBuilder` owns mutable assembly state but never exposes mutable final
instructions.

It accepts the schema-version requirement, declared register/parameter
counts, and limits. It provides:

- `AddConstant(SqlValue)` and `AddSymbol(std::string)`;
- `AddCursor(ReadCursorDescriptor)`;
- `CreateLabel()` and `BindLabel(Label)`;
- `Append(Instruction)` for nonbranch instructions only;
- `EmitJump(Label)`;
- `EmitJumpIf(RegisterId, JumpCondition, Label)`;
- `EmitRewind(CursorId, Label)`;
- `EmitNext(CursorId, Label)`; and
- `EmitSeekRowId(CursorId, RegisterId, Label)`.

Labels are opaque builder-owned values with a unique owner identity and dense
local index. A label may be bound exactly once to the next instruction
address. `Build()` rejects foreign labels, duplicate binds, unbound labels,
labels bound past the final instruction, and resource-limit overflow. It
resolves every label in one pass, creates `ProgramInput`, runs the same
verifier used by direct construction, and consumes the builder.

Moving a builder transfers its owner identity and invalidates the source.
Every operation on a moved-from builder fails with `kInvalidBuilder`, so a
moved-from object cannot mint a duplicate owner/index label.

Final address-bearing branch payloads remain constructible for direct
`ProgramInput` verifier tests. The builder rejects them through `Append`, so
normal lowering cannot bypass label ownership. There is no public
operand-patching API. Lowering expresses forward and backward control flow
with labels and receives an immutable program only after every target is
fixed.

### Allocation and exception boundary

The bytecode layer is internal. Vector and string allocation failure may
throw `std::bad_alloc` during assembly or verification and is converted to
`ErrorCode::kOutOfMemory` by the later compiler/session boundary, consistent
with the syntax-tree and catalog-model construction layers.

No instruction access, constant access, symbol access, cursor-descriptor
access, or sequential program traversal allocates after construction.

### Complexity and performance contract

Assembly and label resolution are O(instructions + labels). Structural
verification is O(instructions). Definite-state verification uses packed
bitsets and deterministic worklists; its cost is proportional to processed
control-flow edges and the declared register and cursor bitset widths.

The executable instruction representation must remain contiguous and no more
than 32 bytes per instruction on the supported Clang/libc++ and GCC/libstdc++
builds. Pinned SQLite's corresponding `VdbeOp` is 24 bytes on the reference
arm64 build.

A deterministic Release benchmark constructs and verifies a 16,384-operation
program with 64 registers, four cursor descriptors, 256 labels, and 1,024
control-flow edges. The benchmark source stored with the result artifact fixes
the instruction mix, branch topology, pool sizes, descriptor shapes, and
correctness checksum. Both implementations use the same process allocator,
`-O3 -DNDEBUG`, 15 warmups, and 101 measured samples; compiler, standard
library, platform, and source hashes are recorded with the result.

Modern SQLite's timed region includes append, label resolution, owned-byte
accounting, structural verification, definite-state analysis, and immutable
publication. Its initial severe-regression gate is 250 ns per instruction at
the median. The benchmark also reports packed analysis words, processed
edge-words, allocations, and final owned bytes.

Pinned SQLite is measured on the same operation and label counts through
`sqlite3VdbeAddOp3()` and `sqlite3VdbeTakeOpArray()`, which resolves labels
without `sqlite3VdbeMakeReady()` register/cursor allocation. That result is
reported as an informational construction baseline, not a ratio gate,
because SQLite does not perform Modern SQLite's typed structural and
definite-state verification. A matched native prepare-time ratio is deferred
until binder, lowering, VM initialization, and session preparation can time
equivalent end-to-end work.

The Node 24 reference run on Apple Clang 21/libc++ arm64 produced a 24-byte
`Instruction` and a Modern SQLite median of 23.94 ns per instruction, passing
the 250 ns gate. The work-matched pinned SQLite assembly baseline produced a
24-byte `VdbeOp` and an informational median of 3.78 ns per instruction. The
benchmark sources, raw results, source hashes, compiler flags, and provenance
are stored in the session artifacts `bytecode_program_benchmark-results.json`,
`sqlite_vdbe_assembly_benchmark-results.json`, and
`bytecode_benchmark-provenance.json`.

Steady-state traversal of a published program must perform zero allocations.

## Verification

Red-first tests cover:

- distinct strong identities, instruction-kind names, move-only program
  ownership, immutable spans, schema-version retention, cursor descriptors,
  and result metadata;
- constants and symbols containing every supported SQL value and UTF-8 name;
- valid straight-line, conditional, loop, arithmetic, affinity, cast,
  empty-result, zero-arity function, table-scan, rowid-seek, WITHOUT ROWID,
  and index-field programs;
- forward and backward label resolution plus foreign, duplicate, unbound, and
  past-end labels;
- every resource limit and checked register-range overflow;
- invalid register, cursor, parameter, constant, symbol, cursor descriptor,
  root page, field mapping, index metadata, and jump references;
- rowid operations on index-backed descriptors;
- result-count mismatches and reachable fallthrough past the program;
- undefined registers across straight-line code and branch joins;
- cursor use before open, reads before positioning, empty/exhausted/missing
  branch positioning, double open, double close, storage-kind misuse, valid
  close-and-reopen, and inconsistent branch-join state;
- unreachable instructions;
- deterministic diagnostics and instruction locations;
- zero-allocation published-program traversal;
- 32-byte maximum instruction footprint on supported toolchains;
- header/source dependency isolation; and
- the fixed-work construction/verification benchmark plus informational
  pinned-SQLite assembly baseline.

Project tests enforce that the bytecode header depends only on base result
and SQL-value contracts. Bytecode source may not include syntax, catalog,
binder, planner, lowering, VM, session, API, diagnostics, format, pager,
B-tree, or VFS headers.

## Consequences

- The VM receives compact, typed, immutable programs and may trust all
  compiler-owned identities, ranges, control-flow targets, and cursor-state
  invariants.
- Lowering uses labels instead of negative addresses or arbitrary operand
  patching.
- Bytecode owns only execution descriptors and a schema-version requirement;
  the prepared statement separately retains the exact catalog snapshot used
  to lower them.
- The instruction variant is larger than pinned SQLite's generic operation
  on the reference build, but it removes `P4` ownership tags, raw pointers,
  and repeated runtime interpretation of operand roles.
- Definite-state verification costs more than SQLite's final label-resolution
  pass. That cost is paid once at publication and guarded by a fixed absolute
  budget plus an informational native assembly baseline.
- Program construction remains move-only because constants include move-only
  `SqlValue` objects.
- Unknown runtime function and collation registrations remain explicit VM
  errors rather than verifier guesses or silent fallbacks.
- Later write, aggregate, sort, join, and coroutine instructions extend the
  typed variant and verifier deliberately instead of inheriting unused VDBE
  semantics now.

## Alternatives considered

### Mirror SQLite `VdbeOp`

Rejected. Generic integer operands and the ownership-tagged `P4` union make
invalid combinations representable, expose pointer lifetime rules to the
compiler and VM, and preserve an internal format SQLite does not stabilize.

### Execute plans or AST nodes directly

Rejected. That would couple the VM to parser, binder, and planner objects,
prevent an independently testable lowering boundary, and violate the module
DAG.

### Retain catalog IDs and the catalog snapshot in bytecode

Rejected. The current catalog contract owns parsed DDL syntax trees, so a
catalog dependency would give bytecode and the VM a transitive AST dependency
and contradict the architecture graph. Lowering instead copies the minimal
root, record-layout, ordering, symbol, and schema-version metadata into
bytecode-owned cursor descriptors while the prepared statement retains the
catalog snapshot separately.

### Heap-allocated polymorphic instruction objects

Rejected. One allocation and virtual dispatch per instruction are unnecessary
for a compact high-frequency execution stream.

### Encode labels as negative addresses

Rejected. The same integer would represent two domains and require mutation
or sentinel interpretation during finalization.

### Require every halt path to close every cursor explicitly

Rejected. Cursor slots are VM-owned resources that must be closed on errors,
reset, and destruction regardless of bytecode shape. Requiring explicit
close only for successful halt paths would not remove the runtime cleanup
requirement and would complicate early exits.

### Introduce SSA bytecode

Rejected for the current MVP. SSA would improve some compiler analyses but
would add phi construction, lowering, destruction, and register assignment
before the basic read execution path exists.
