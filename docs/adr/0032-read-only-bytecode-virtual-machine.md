# ADR-0032: Read-Only Bytecode Virtual Machine

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0031 defines compact, typed, immutable bytecode programs. The next
dependency-graph node must execute hand-authored programs before binder,
planner, lowering, and prepared-statement APIs exist. The executor must cover
the complete initial instruction set: constants, parameters, expression
operations, read cursors, rowid lookup, record fields, scalar functions,
control flow, result suspension, and successful halt.

Pinned SQLite 3.54.0 provides the behavioral reference:

- `src/vdbeInt.h:397-488` keeps registers, bound variables, cursor slots,
  result state, and the program counter in one VM object;
- `src/vdbe.c:902-1040` dispatches from the saved program counter and counts
  executed instructions;
- `src/vdbe.c:1427-1828` loads constants and variables, deep-copies values,
  and suspends after `OP_ResultRow`;
- `src/vdbe.c:1851-2144` implements concatenation, numeric arithmetic,
  overflow-to-REAL behavior, division by zero, remainder, bit operations, and
  defined 64-bit shifts;
- `src/vdbe.c:2640-2828` implements three-valued `AND`, `OR`, `NOT`, and
  conditional branches through SQLite numeric truth conversion;
- `src/vdbe.c:3035-3520` decodes record fields and returns NULL for a missing
  physical field;
- `src/vdbe.c:4529-4650`, `src/vdbe.c:4850-4860`,
  `src/vdbe.c:5638-5715`, `src/vdbe.c:6309-6355`, and
  `src/vdbe.c:6530-6710` open, close, seek, read, rewind, and advance B-tree
  cursors;
- `src/vdbe.c:9034-9098` invokes a previously resolved scalar function;
- `src/vdbeaux.c:2804-2860` closes every remaining cursor on halt, reset, or
  destruction; and
- `src/vdbeaux.c:3597-3635` resets a halted or suspended VM to its ready
  state while retaining parameter bindings.

Modern SQLite deliberately separates responsibilities that SQLite combines:

- the immutable `BytecodeProgram` owns compiler output;
- `ReadPager` owns the active database snapshot and requires external
  transaction serialization;
- table and index B-tree cursors own page pins and detect snapshot changes;
- `RecordView` validates and decodes SQLite records;
- `FunctionRegistry` and `Collation` provide stable runtime behavior;
- the VM must not depend on syntax, catalog, binder, plan, optimizer,
  lowering, session, API, or diagnostics types; and
- the future prepared statement owns the program, retained catalog snapshot,
  parameter API, and read-transaction lifecycle.

The executor is performance-sensitive. Verification has already proved
register, cursor, range, branch, reachability, and cursor-state invariants.
The dispatch loop must trust those compiler-owned invariants instead of
repeating recoverable validation on every instruction, while still reporting
database, registration, schema, resource, and I/O failures explicitly.

## Decision

Add the read-only executor under:

- `include/modern_sqlite/vm/read_vm.hpp`; and
- `src/vm/read_vm.cpp`.

### Public execution boundary

The public contract is move-only and hides runtime storage behind an
implementation pointer:

```cpp
enum class ReadVmState : std::uint8_t {
  kReady,
  kRow,
  kDone,
  kError,
  kInvalid,
};

enum class ReadVmStep : std::uint8_t {
  kRow,
  kDone,
};

struct ReadVmLimits {
  std::size_t maximum_value_bytes = 1'000'000'000;
  std::uint64_t maximum_instructions_per_step =
      std::numeric_limits<std::uint64_t>::max();
};

class ReadVmEnvironment final {
 public:
  ReadVmEnvironment(
      ReadPager& pager,
      std::uint64_t catalog_generation,
      const FunctionRegistry& functions,
      std::span<const Collation* const> collations) noexcept;

  static ReadVmEnvironment Core(
      ReadPager& pager,
      std::uint64_t catalog_generation) noexcept;
};

class ReadVm final {
 public:
  static Result<ReadVm> Create(
      const BytecodeProgram& program,
      ReadVmEnvironment environment,
      ReadVmLimits limits = {});
  static Result<ReadVm> Create(
      BytecodeProgram&& program,
      ReadVmEnvironment environment,
      ReadVmLimits limits = {}) = delete;
  static Result<ReadVm> Create(
      const BytecodeProgram&& program,
      ReadVmEnvironment environment,
      ReadVmLimits limits = {}) = delete;

  ReadVm(ReadVm&&) noexcept;
  ReadVm& operator=(ReadVm&&) noexcept;
  ~ReadVm();

  Status Bind(ParameterId parameter, const SqlValue& value);
  Status ClearBindings();
  Result<ReadVmStep> Step();
  Status Reset();

  ReadVmState state() const noexcept;
  std::span<const SqlValue> row() const noexcept;
  std::uint64_t executed_instruction_count() const noexcept;
};
```

`ReadVm` borrows the program, pager, resolved scalar-function descriptors, and
every resolved collation. The program and pager must remain alive at stable
addresses and must not be moved while the VM exists. Rvalue programs are
rejected at compile time. The future prepared statement keeps its program at a
stable address, for example in unique ownership, so moving the statement does
not invalidate a live VM.

`FunctionRegistry` is itself a borrowed span. The descriptor array and every
borrowed function-name string behind that registry must therefore outlive the
VM, not merely the registry wrapper. Likewise, each resolved `Collation`
object must outlive the VM. The environment object, its collation-pointer
container, and the registry wrapper need remain valid only during the
synchronous `Create()` call because the VM copies the resolved pointers.

The VM owns registers, parameter values, cursor slots, record scratch
storage, resolved runtime handles, and execution state.

`ReadVmEnvironment::Core()` uses the process-wide core function registry and a
static pointer array containing the `BINARY`, `NOCASE`, and `RTRIM` singleton
collations. An explicit environment may supply a connection-owned immutable
function registry and a borrowed collation list. Collation names use SQLite
ASCII case folding, and the first matching descriptor wins. Null collation
pointers are misuse.

The VM public header includes only its direct contracts and uses forward
declarations where practical. Project tests reject dependencies on syntax,
catalog, binder, logical-plan, optimizer, lowering, session, API, and
diagnostics headers.

### Initialization and atomic schema validation

`ReadVm::Create()` requires an active `ReadPager` read transaction. It reads
the schema cookie from the pager header, using zero for an empty database, and
compares:

- that cookie with `BytecodeProgram::schema_version().schema_cookie`; and
- the environment's retained catalog generation with
  `BytecodeProgram::schema_version().generation`.

Either mismatch returns `kSchemaChanged`. The check occurs before runtime
symbol resolution or cursor opening. Creation leaves the VM ready but not
attached permanently to that pager snapshot.

The first `Step()` from `kReady` begins one execution lifetime. It repeats the
schema-cookie and catalog-generation checks in the caller's then-active read
transaction and captures `ReadPager::data_version()`. Every resumed `Step()`
from `kRow` requires that same transaction and snapshot identity. The
execution therefore cannot switch snapshots while cursors or result values
are live.

Successful halt or execution error ends that execution lifetime. `Reset()`
also closes resources and detaches the VM from the completed snapshot. A
later `Step()` from `kReady` may attach to a new data snapshot after an
ordinary data-only change, provided the schema cookie and retained catalog
generation still match the immutable program. A schema change still returns
`kSchemaChanged` and requires session-level refresh or recompilation.

The VM does not begin or end transactions. The future session layer begins or
joins the read transaction before creation, keeps it active across every row
suspension, resets or destroys the VM before ending a statement-owned
transaction, and supplies the generation of the retained catalog snapshot.
This node therefore does not create an early dependency on the later
transaction coordinator.

Creation performs all runtime name resolution:

- every collation used by an index descriptor, comparison, or scalar call is
  resolved once to a stable `const Collation*`; and
- every scalar call is resolved once by name and argument count to a stable
  `const ScalarFunction*`.

Unknown collations, unknown functions, and invalid function arities are
creation errors. The dispatch loop never repeats name lookup. Resolved
collations use a symbol-indexed pointer array. Resolved scalar calls use a
compact address-sorted array so programs do not pay one pointer for every
non-call instruction.

Registers and parameter slots are allocated once. Parameter slots initially
contain SQL NULL, matching SQLite's unbound-parameter behavior. Cursor slots
are initially closed. The program has already been verified, so creation does
not repeat bytecode verification.

### State machine, rows, bindings, and reset

The state transitions are:

```text
Create -> Ready
Ready --Step--> Row | Done | Error
Row   --Step--> Row | Done | Error
Ready | Row | Done | Error --Reset--> Ready
move-from -> Invalid
```

`Step()` in `Done`, `Error`, or `Invalid` is misuse. `Bind()` and
`ClearBindings()` are accepted only in `Ready`. `Bind()` checks the parameter
ID and value-size limit, then clones the supplied value so the caller retains
independent ownership.

`LoadParameterInstruction` clones the retained binding into its destination
register. Register mutations therefore never modify bindings.

`ResultRowInstruction`:

- exposes a const span over the selected register range;
- saves the following instruction as the resume address;
- enters `kRow`; and
- returns `ReadVmStep::kRow` without closing cursors.

The row remains valid only until the next `Step()`, `Reset()`, move operation,
or destruction. A zero-column result is represented by `kRow` plus an empty
span, so it remains distinguishable from no current row.

`HaltInstruction` closes all cursors, invalidates the current row, enters
`kDone`, and returns `ReadVmStep::kDone`. Any execution error closes all
cursors, invalidates the row, enters `kError`, and returns the error. The
destructor also closes every cursor through ordinary RAII.

`Reset()` closes every cursor, destroys register-owned dynamic values, clears
the current row, restores the program counter to zero, resets the executed
instruction count, detaches from the completed pager snapshot, and enters
`kReady`. It retains parameter bindings and resolved function and collation
handles. The next `Step()` performs a fresh atomic schema check and captures
the then-current data snapshot. `ClearBindings()` explicitly restores every
parameter to NULL.

This explicit state machine does not implement SQLite's public
`sqlite3_step()` automatic-reset compatibility behavior. The later session
and C API layers decide whether to expose automatic reset.

### Cursor slots and record caching

Each runtime cursor slot contains:

- either no cursor, a `TableBtreeCursor`, or an `IndexBtreeCursor`;
- an optional owned complete record for overflow-backed payloads; and
- an optional parsed `RecordView` that borrows the current local or owned
  payload; and
- a reusable vector of decoded `RecordFieldView` values for the physical
  fields exposed at that cursor position.

`OpenReadCursorInstruction` converts the bytecode root page to `PageNumber`
and opens the descriptor's exact storage kind. Index metadata is converted to
`IndexColumnOrder` using already resolved collations. For a nonempty database,
the VM uses the shared database-format normalization contracts:

- raw schema format zero is normalized to format one;
- formats one through four select the corresponding `RecordCodecOptions`;
- the low two encoding bits normalize zero and one to UTF-8; and
- normalized UTF-16 encodings return `kProtocol` until the text and record
  layers provide transcoding.

Other schema formats are `kNotDatabase`. The normalized record options are
passed explicitly to every table-record parse. Empty databases retain the
B-tree cursor contract from ADR-0024.

`RewindInstruction`, `NextInstruction`, and `SeekRowIdInstruction` clear the
slot's record cache before repositioning. Their branch behavior matches the
verified edge model:

- rewind branches when the tree is empty and otherwise falls through at the
  first entry;
- next jumps after a successful advance and falls through after exhaustion;
  and
- rowid seek falls through on an exact hit and branches on conversion failure
  or a missing row.

`ReadFieldInstruction` first examines the descriptor's exact
`CursorFieldSource`:

- `kRowId` reads the current table rowid directly and does not parse or touch
  the record cache; and
- `kRecordField` obtains or reuses the decoded current record.

A local payload is parsed without copying. An overflow-backed payload is
copied once per cursor position and reused by later field reads. On the first
physical-field read at a position, one `RecordCursor` pass decodes every
physical field needed by the descriptor into the reusable
`RecordFieldView` vector. Later field instructions are indexed lookups rather
than rescans from field zero. Each header entry is therefore decoded at most
once after structural parsing, avoiding quadratic projection work.

A requested physical field beyond the stored record follows the descriptor's
ADR-0036 missing-field policy:

- return SQL NULL;
- clone an affinity-applied program constant; or
- fail explicitly because the catalog default is unsupported.

A stored NULL remains NULL and never triggers substitution. Malformed records
and payload failures propagate explicitly.

`ReadRowIdInstruction` reads the current table rowid. The bytecode verifier
already prevents rowid operations on index-backed descriptors.

Closing, exhausting, failing, resetting, halting, or destroying a cursor slot
destroys every borrowed `RecordView` before releasing or moving the cursor
that owns its page pins.

### Value and expression semantics

Constants, parameters, and copies produce independent register-owned values.
Numeric and NULL values copy without heap allocation; TEXT and BLOB use
explicit `SqlValue::Clone()`.

The SQL-value layer adds one narrow helper for arithmetic numeric coercion.
It preserves INTEGER and REAL inputs, propagates NULL, and converts TEXT or
BLOB using the numeric prefix and result type selected by SQLite's VDBE
arithmetic path. This differs intentionally from forced `CAST AS NUMERIC`:
for example, arithmetic keeps `'1.0'` as REAL while a numeric cast may produce
INTEGER.

Every register-writing instruction reads or clones all inputs before assigning
its output. Input/output aliasing is therefore deterministic:

| Instruction | Output commit rule |
|---|---|
| load constant | clone the immutable constant, then replace the output |
| load parameter | clone the retained binding, then replace the output |
| copy | clone the source before replacing the destination |
| unary | finish conversion and evaluation before replacing the output |
| binary | finish both input conversions and evaluation before replacement |
| apply affinity | clone and convert before replacement |
| cast | clone and convert before replacement |
| read field / rowid | obtain one owned result before replacement |
| compare | finish comparison before writing INTEGER or NULL |
| scalar call | finish callback invocation before replacing the output |

This rule applies when the output equals any input or scalar-call argument.

Unary instructions behave as follows:

- negate performs SQLite numeric coercion, preserves integer results when
  negation fits, falls back to REAL for the minimum signed integer, and
  propagates NULL;
- bitwise NOT converts through SQLite integer-prefix semantics and propagates
  NULL; and
- logical NOT uses SQLite numeric truth conversion and propagates NULL.

Binary arithmetic:

- propagates NULL;
- performs integer add, subtract, and multiply when both coerced operands are
  INTEGER and the exact result fits;
- falls back to binary64 REAL on integer overflow;
- performs integer division when both operands are INTEGER;
- returns NULL for integer or REAL division by zero;
- handles minimum-integer divided by negative one through REAL fallback;
- implements remainder through integer conversion, returns INTEGER when both
  operands were INTEGER and REAL otherwise, and returns NULL for zero
  divisors;
- treats minimum signed integer remainder negative one as zero without
  evaluating the undefined C++ expression; and
- constructs REAL results through `SqlValue::Real()`, so NaN becomes NULL
  while infinities remain REAL.

Concatenation converts non-NULL operands to their SQLite UTF-8 text
representations, preserves exact BLOB bytes when interpreted as text,
concatenates left then right, checks length before allocation, and returns
NULL when either operand is NULL.

Bitwise AND, OR, left shift, and right shift use SQLite signed 64-bit integer
conversion. Negative shift counts reverse direction. Counts of 64 or more
produce zero for left shift and nonnegative right shift, or negative one for
right shift of a negative value. Count normalization checks the minimum signed
integer before negation and caps it at 64, matching SQLite without signed
overflow. Unsigned intermediates avoid undefined C++ signed-shift behavior.

Logical AND and OR implement SQLite's three-valued truth tables:

- false dominates AND;
- true dominates OR;
- otherwise a NULL operand produces NULL.

`ApplyAffinityInstruction` and `CastInstruction` apply the existing
loss-avoiding affinity and forced-cast contracts to a clone of the input.

ADR-0036 adds two narrow conversions:

- `MustBeIntegerInstruction` applies numeric affinity, accepts only a
  losslessly representable signed 64-bit integer, and returns
  `kTypeMismatch` with `datatype mismatch` otherwise; and
- `RealAffinityInstruction` converts INTEGER to REAL while leaving NULL,
  REAL, TEXT, and BLOB unchanged.

Both commit their output only after successful conversion or cloning and
support aliased input/output registers.

`CompareInstruction` uses a comparison-specific coercion routine rather than
ordinary affinity applied blindly to both values:

- two INTEGER values compare directly before affinity;
- NULL handling for ordinary comparisons and `IS` / `IS NOT` occurs before
  coercion;
- numeric, integer, or real comparison affinity attempts numeric conversion
  only for TEXT operands and uses SQLite's comparison numeric conversion;
- text affinity performs text conversion only when at least one operand is
  already TEXT, and then only converts a numeric opposite operand;
- BLOB values are never converted by comparison affinity; and
- collation dispatch occurs only when both post-coercion operands are TEXT.

Borrowed inputs remain unchanged. Temporary owned values are created only for
the operands SQLite would actually convert, so integer/integer, text/text,
numeric/numeric, BLOB, and NULL fast paths allocate nothing. The instruction
writes INTEGER zero, INTEGER one, or NULL only after comparison completes.

`CallScalarInstruction` passes a borrowed contiguous argument span and the
resolved selected collation to the resolved function. The returned
`SqlValue` becomes the output only after successful invocation, so an output
register may overlap the argument range safely.

`JumpIfInstruction` uses SQLite numeric truth conversion:

- `kIfTrue` branches only for true;
- `kIfFalse` branches only for false;
- `kIfNull` branches only for NULL; and
- `kIfNotNull` branches only for non-NULL.

NULL therefore takes neither the true nor false branch.

`SeekRowIdInstruction` does not mutate its key register. INTEGER is used
directly. TEXT receives lossless numeric affinity. REAL is accepted only when
it is finite, within SQLite's exactly convertible signed-64-bit range, and
equal to the converted integer. NULL, BLOB, fractional REAL, malformed TEXT,
and out-of-range values take the missing branch.

### Runtime limits and interruption

`maximum_value_bytes` defaults to SQLite's 1,000,000,000-byte length limit. It
applies to:

- bound TEXT and BLOB parameters;
- program constants when the VM is created;
- concatenation results;
- decoded record fields and copied overflow records;
- cast and affinity outputs; and
- scalar-function outputs.

Operations reject known oversize results with `kTooLarge` before allocation
where the exact size is available.

`maximum_instructions_per_step` bounds work performed by one `Step()` call.
The default is effectively unlimited for SQLite-compatible ordinary
execution. A caller may set a finite budget for deterministic cancellation or
untrusted hand-authored programs. Exhaustion returns `kInterrupted`, closes
all cursors, and enters `kError`. The budget restarts after each row
suspension, while `executed_instruction_count()` records the cumulative count
since creation or reset.

Every dispatched instruction records
`instrumentation::Counter::kVmInstructions` when optional instrumentation is
enabled. The disabled production build retains zero instrumentation cost.

### Error and exception contract

The VM returns the existing `Result` and `Status` aliases.

- invalid states, moved-from objects, invalid environment data, inactive
  transactions, and invalid binding IDs are `kMisuse`;
- schema cookie, catalog generation, or pager snapshot mismatches are
  `kSchemaChanged`;
- unknown functions, wrong arity, and unknown collations preserve explicit
  runtime resolution errors;
- runtime value or record limits are `kTooLarge`;
- instruction-budget exhaustion is `kInterrupted`;
- invalid database encoding or format uses the existing `kProtocol` and
  `kNotDatabase` contracts;
- record, cursor, pager, cache, VFS, and function failures propagate their
  typed errors unchanged.

`ReadVm::Create()`, `Bind()`, and `Step()` are allocation boundaries.
`std::bad_alloc` is converted to `Error::OutOfMemory()`.

`ScalarFunctionCallback` is not declared `noexcept`. If any other exception
escapes a callback or internal operation, `Step()` first invalidates the row,
closes every cursor, ends the current execution lifetime, and enters
`kError`, then rethrows the original exception. The catch-all exists only to
restore deterministic RAII-visible state; it never converts an unknown
failure into an ordinary database result or success.

The bytecode verifier is the trust boundary for compiler-owned instruction
invariants. The VM may assert valid register, symbol, cursor, field, range,
branch, and state transitions established by `BytecodeProgram::Create()`.
It must not turn malformed external database bytes, unavailable runtime
registrations, inactive snapshots, or callback errors into assertions.

## Explicitly deferred behavior

- Parser, binder, planner, optimizer, and lowering integration.
- Prepared-statement ownership, named parameter lookup, and public binding
  overloads.
- Automatic transaction begin/end and connection-level transaction joining.
- Automatic reset compatibility in the public `step()` API.
- Index seeks, reverse scans, sorting, joins, aggregates, windows,
  coroutines, subprograms, and ephemeral tables.
- Writes, journals, savepoints, triggers, foreign keys, and schema mutation.
- Lazy special forms such as `coalesce`, `ifnull`, and `iif`; lowering will
  express them with control flow when the binder supports them.
- Stateful application functions, auxiliary data, subtypes, and
  non-deterministic session functions.
- UTF-16 database text decoding and collation adapters.
- Progress callbacks, asynchronous interrupt flags, and connection-wide
  cancellation policy.
- Streaming a single record field directly from overflow pages.

## Validation plan

Red-first tests cover:

- creation only inside an active matching pager snapshot;
- schema-cookie, catalog-generation, and data-version mismatches;
- case-insensitive built-in and custom collation resolution;
- function resolution by exact and minimum arity and explicit missing
  registration failures;
- NULL default bindings, binding clones, retained bindings across reset, and
  clearing bindings;
- row suspension, zero-column rows, resume addresses, halt, invalid repeated
  step calls, moved-from objects, and reset from every state;
- all unary, binary, affinity, cast, comparison, scalar-call, and jump
  instructions;
- pinned SQLite differential vectors for integer overflow, numeric text,
  invalid numeric text, division and remainder by zero, minimum integers,
  concatenation, signed shifts, and three-valued logic;
- table scans, index scans, rowids, local and overflow-backed fields, missing
  physical fields, rowid seek hit/miss conversion, and record-cache
  invalidation on movement;
- cursor cleanup on halt, error, reset, and destruction, including pager
  `EndRead()` behavior across a suspended row;
- value limits, instruction budgets, allocation failure, corruption, and I/O
  propagation;
- optional VM-instruction instrumentation; and
- VM dependency-layer enforcement.

The deterministic performance baseline uses the pinned B-tree fixture and a
hand-authored program equivalent to:

```sql
SELECT id, binary_key FROM cursor_sample;
```

Modern SQLite uses one verified program with `ReadVm::Step()` and
`ReadVm::Reset()`. Pinned SQLite uses one prepared statement with
`sqlite3_step()` and `sqlite3_reset()`. Both use an explicit read transaction,
a warm page cache, the same 166 rows, and verification outside timed loops.
The benchmark reports median nanoseconds per row, VM instructions,
allocations, pages read, and checksum.

The SQLite scan number is informational in Node 25 because public
`sqlite3_step()` includes connection mutex and API machinery that the internal
Modern VM boundary does not yet have. A ratio gate is deferred until the read
session API can compare equivalent public execution boundaries.

The Modern scan has an absolute severe-regression guard of 2,500 nanoseconds
per row on the reference Apple arm64 Release configuration. A separate
register-only dispatch benchmark must remain at or below 250 nanoseconds per
instruction and perform no allocation after VM initialization. Benchmark
provenance records source identity, compiler, flags, platform, executable
hashes, samples, and raw results.

Full validation includes Debug, Release, ASan/UBSan, TSan, clang-tidy,
formatting, project-graph checks, and all project-layering tests.

## Consequences

- The next binder, planner, and lowering nodes have an executable target with
  no dependency on their internal representations.
- The future session can validate one retained catalog snapshot and keep one
  VM suspended safely across result rows.
- Runtime name lookup, bytecode validation, and cursor-kind decisions remain
  outside the hot dispatch path.
- RAII cleanup makes cursor release independent of successful bytecode
  cleanup paths.
- Register and result ownership are explicit; no TEXT or BLOB pointer aliases
  escape into mutable caller storage.
- The initial VM remains intentionally read-only and small enough to audit.

## Alternatives considered

### Execute AST or plan nodes directly

Rejected. It would couple runtime execution to parser, binder, or optimizer
ownership and eliminate the independently verifiable lowering boundary.

### Let the VM begin and end pager transactions

Rejected. Transaction ownership belongs to the prepared statement and later
connection transaction coordinator. The VM must remain usable inside either
a statement-owned or caller-owned read transaction.

### Resolve functions and collations at every instruction

Rejected. Registrations are stable for one execution, and repeated
case-insensitive lookup would add avoidable hot-path work and permit behavior
to change under a suspended program.

### Store resolved pointers in the immutable bytecode

Rejected. Bytecode is connection-independent compiler output. Function and
collation registrations are connection/runtime state with different
lifetimes and invalidation rules.

### Copy every result row

Rejected. Registers already own stable values until execution resumes.
Borrowing the row matches SQLite's column lifetime, avoids per-row container
allocation, and keeps the invalidation boundary explicit.

### Require bytecode to close every cursor before halt

Rejected for the same reason as ADR-0031. Errors, cancellation, reset, and
destruction still require VM-owned cleanup, so explicit successful-path
closes cannot replace RAII cleanup.

### Re-decode or re-copy the record for every field instruction

Rejected. Multiple projected fields from one row are the common path.
Caching one parsed local view or one complete overflow copy per cursor
position preserves safety while avoiding repeated record and overflow work.

### Impose a finite default instruction budget

Rejected. SQLite does not impose an arbitrary per-step VM limit. The explicit
limit remains available for deterministic cancellation and hostile
hand-authored programs without changing ordinary execution semantics.
