# ADR-0037: RAII Read Session API

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0030 loads immutable catalog snapshots, ADR-0033 through ADR-0036
compile one supported SELECT into verified bytecode, and ADR-0032 executes
that bytecode against one active pager snapshot. The next dependency-graph
node must compose those lower layers into the first application-facing API:

> RAII database and prepared-statement API for open, prepare, bind, step,
> reset, and finalize.

The lower layers intentionally do not own connection lifecycle:

- `ReadPager::Open()` opens a read-only file without validating its header;
- `ReadPager::BeginRead()` and `EndRead()` define snapshot lifetime;
- catalog loading and reload detection require an active read transaction;
- binding consumes one immutable catalog snapshot;
- lowering produces a program that owns result metadata but not parameter
  names;
- `Vm` permanently borrows its program and borrows the pager only through an
  attached execution context;
- `Vm::Step()` requires an attached context, while only snapshot-dependent
  programs require an active pager transaction;
- `Vm::Reset()` closes cursors, detaches the context, and preserves bindings;
  and
- VM row spans remain valid only while execution is suspended on a row.

The session layer must therefore own and coordinate:

- the VFS, pager, current catalog, and catalog generation;
- SQL text retained for automatic reprepare;
- stable bytecode storage that outlives each VM;
- parameter names that disappear after planning;
- shared transaction lifetime across simultaneously active statements;
- schema refresh and automatic statement recompilation;
- public statement states and SQLite-compatible error reporting; and
- deterministic cleanup when explicit finalization or destructors run.

Pinned SQLite 3.54.0 provides the lifecycle reference:

- `src/main.c:1273-1408` defines busy and zombie connection closure;
- `src/main.c:3388-3770` defines open timing and failure ownership;
- `src/prepare.c:703-1000` defines prepare tails, retained SQL, and automatic
  reprepare;
- `src/vdbeapi.c:105-174` defines finalize, reset, and clear-bindings
  behavior;
- `src/vdbeapi.c:980-1120` defines step and automatic reset;
- `src/vdbeapi.c:1695-2060` defines one-based binding; and
- generated `sqlite3.h:4313-4660`, `4908-5035`, `5275-5385`, and
  `5620-5695` document the public statement lifecycle.

Focused probes against the pinned amalgamation established the supported
behavioral baseline:

- read-only open of a missing file returns `SQLITE_CANTOPEN`;
- open of a corrupt file succeeds and prepare returns `SQLITE_NOTADB`;
- a zero-byte file is a valid empty database;
- prepare returns the first statement and a byte tail offset;
- only trivia and empty statements return no statement;
- parameter indices are one-based;
- repeated named or numbered parameters share one slot;
- the first spelling of a numbered parameter is retained, so `?0001` and
  `?1` share slot one while lookup remains exact;
- unbound parameters are NULL;
- bindings survive reset and automatic schema reprepare;
- binding while a statement is active returns `SQLITE_MISUSE`;
- reset and finalize return the prior execution error;
- step after DONE or ERROR automatically resets and starts a new execution;
- prepare does not retain a read transaction;
- multiple active statements share the connection read transaction;
- prepare may run while another statement is suspended;
- the read transaction ends when the last storage-reading statement reaches
  DONE, errors, resets, or finalizes;
- `SELECT 1` neither starts nor retains a read transaction;
- `prepare_v2`/`prepare_v3` automatically recompile after a schema change;
- result metadata changes when automatic reprepare changes `SELECT *`; and
- `sqlite3_close_v2()` allows prepared statements to outlive the public
  connection handle.

Modern SQLite does not need to reproduce unsafe C pointer conventions,
undefined double-finalize behavior, or connection-global mutable error
buffers. It must preserve supported SQL results, primary error codes,
statement state transitions, binding persistence, schema behavior, and
resource lifetime.

This node remains read-only and deliberately excludes:

- writes, explicit transactions, savepoints, WAL reads, or shared-cache mode;
- user-defined functions or collations;
- statement caching;
- concurrent calls through one session state;
- `sqlite3_clear_bindings()` and transfer-bindings convenience APIs;
- expanded SQL, normalized SQL, tracing, profiling, or progress callbacks;
- origin metadata and deprecated table/column metadata APIs; and
- a broad connection-configuration surface.

## Decision

Add the session module under:

- `include/modern_sqlite/session/read_session.hpp`; and
- `src/session/read_session.cpp`.

The public API consists of a move-only `ReadSession`, a move-only
`ReadStatement`, one prepare output, and one step result.

### Public contract

```cpp
enum class ReadStep : std::uint8_t {
  kRow,
  kDone,
};

class ReadStatement final {
 public:
  ReadStatement(const ReadStatement&) = delete;
  ReadStatement& operator=(const ReadStatement&) = delete;
  ReadStatement(ReadStatement&&) noexcept;
  ReadStatement& operator=(ReadStatement&&) noexcept;
  ~ReadStatement();

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] std::size_t parameter_count() const noexcept;
  [[nodiscard]] std::optional<std::string_view> parameter_name(
      std::size_t parameter_index) const noexcept;
  [[nodiscard]] std::size_t parameter_index(
      std::string_view parameter_name) const noexcept;

  [[nodiscard]] std::span<const ResultColumnMetadata> result_columns()
      const noexcept;
  [[nodiscard]] std::span<const SqlValue> row() const noexcept;

  [[nodiscard]] Status Bind(std::size_t parameter_index,
                            const SqlValue& value);
  [[nodiscard]] Result<ReadStep> Step();
  [[nodiscard]] Status Reset();
  [[nodiscard]] Status Finalize();

 private:
  friend class ReadSession;

  struct Impl;

  explicit ReadStatement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct ReadPrepareOutput {
  std::optional<ReadStatement> statement;
  ByteOffset next_offset;
};

class ReadSession final {
 public:
  [[nodiscard]] static Result<ReadSession> Open(std::string_view path);
  [[nodiscard]] static Result<ReadSession> Open(
      std::unique_ptr<Vfs> vfs, std::string_view path);

  ReadSession(const ReadSession&) = delete;
  ReadSession& operator=(const ReadSession&) = delete;
  ReadSession(ReadSession&&) noexcept;
  ReadSession& operator=(ReadSession&&) noexcept;
  ~ReadSession();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<ReadPrepareOutput> Prepare(Utf8View source);

 private:
  friend class ReadStatement;

  struct State;

  explicit ReadSession(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
};
```

The default `Open()` owns a `PosixVfs`. The overload accepting
`std::unique_ptr<Vfs>` transfers VFS ownership and supports deterministic
tests and future embedders without a borrowed-lifetime hazard. A null VFS is
misuse.

The session API uses the existing default limits of the parser, catalog
loader, binder, program builder, lowering layer, pager, and VM. This node
does not duplicate all lower-layer options in a premature connection-options
type. A later node may add a coherent options object when a real application
requires configuration.

### Ownership and closure

`ReadSession::State` owns objects in this lifetime order:

1. one VFS;
2. one `ReadPager` borrowing that VFS;
3. the current immutable catalog snapshot; and
4. transaction and generation bookkeeping.

Both `ReadSession` and every `ReadStatement` hold `std::shared_ptr<State>`.
Destroying or moving from the public session handle prevents new prepares
through that handle but does not invalidate existing statements. The state,
pager, file, and VFS are destroyed after the final session or statement
handle releases them. This is the safe C++ equivalent of
`sqlite3_close_v2()` zombie ownership.

There is no explicit `ReadSession::Close()` in this node. A close operation
whose only observable effect is releasing one shared owner would duplicate
the destructor without providing SQLite's unsafe busy-close distinction.

Each statement implementation owns:

- one shared state pointer, declared before the execution object so the
  borrowing VM and program are destroyed before the final state owner;
- the exact consumed SQL bytes needed for reprepare;
- parameter-name metadata;
- a heap-stable `BytecodeProgram`;
- a `Vm` borrowing that program;
- its public lifecycle state;
- any retained terminal error; and
- whether it currently contributes to the shared read-transaction count.

The program and VM live in one heap-owned execution object with the program
declared before the VM. Destruction therefore destroys the borrowing VM
before the borrowed program. Automatic reprepare builds a complete
replacement execution object, copies bindings into it, and swaps it only
after every operation succeeds.

Move construction and move assignment preserve statement identity and row
lifetime because the implementation and execution objects remain at stable
heap addresses. A moved-from session or statement is invalid. Operations
other than `valid()`, metadata queries, `row()`, and idempotent `Finalize()`
return misuse on an invalid object.

### Open and prepare timing

`ReadSession::Open()`:

1. validates VFS ownership;
2. calls `ReadPager::Open()` in read-only mode;
3. allocates shared state; and
4. performs no header read, catalog load, or transaction.

Consequently:

- a missing or inaccessible path fails during open;
- a corrupt nonempty file may open successfully;
- a zero-byte file opens successfully; and
- corruption, unsupported WAL state, and schema errors surface during
  prepare, matching pinned SQLite's lazy validation boundary.

`Prepare()` first calls `ParseOne()` without opening a pager transaction.
This preserves prepare-style first-statement slicing and avoids storage work
for trivia-only input or a lexical/syntax error.

If parsing succeeds with no statement, `Prepare()` returns:

```cpp
ReadPrepareOutput{
    .statement = std::nullopt,
    .next_offset = parsed.next_offset,
};
```

For a statement, prepare then enters the session's current snapshot or opens
a temporary read transaction, refreshes the catalog, and runs exactly:

```text
SyntaxTree
  -> BindSelectStatement
  -> BuildLogicalPlan
  -> OptimizeLogicalPlan
  -> LowerReadPlan
  -> Vm::Create
```

The statement stores source bytes from offset zero through
`next_offset`. Leading trivia and the terminating semicolon are therefore
retained exactly, while later statements in the same input are not.

If no storage-reading statement is active, prepare owns a temporary read
transaction and must end it before returning. If another statement already
holds the session snapshot, prepare joins that snapshot and does not change
the shared transaction count.

Prepare publishes no partially compiled statement. Failure destroys all
temporary syntax, plans, programs, and VMs before returning the mapped
`Error`.

`Vm::Create()` resolves runtime functions and collations while prepare still
has its temporary catalog snapshot, but it does not retain the Pager or that
transaction. Execution attaches the then-current Pager and catalog generation
immediately before the first step.

### Catalog snapshots and generations

The state caches one `CatalogSnapshotPtr` and a monotonically increasing
64-bit generation. The first successful load uses generation one.

At every snapshot-entry boundary:

1. if no catalog exists, load it;
2. otherwise call `CatalogRequiresReload()`;
3. if the schema cookie changed, load a complete replacement with the next
   generation; and
4. publish the replacement only after loading succeeds.

Generation overflow returns `ErrorCode::kTooLarge` rather than wrapping.
The old catalog remains published after any failed reload.

The catalog snapshot is immutable. Preparing while another statement is
suspended uses the exact same pager snapshot and catalog generation as that
statement.

### Read-transaction ownership

The state tracks the number of active statements whose bytecode requires a
database snapshot.

A statement acquires one shared execution reference immediately before its
first VM step. The first such reference begins the pager transaction and
refreshes the catalog. Later references join the existing transaction.

A statement releases its reference when it:

- returns DONE;
- returns an execution error;
- resets from ROW;
- finalizes from ROW; or
- is destroyed while active.

The final release calls `ReadPager::EndRead()`. Statements suspended on ROW
therefore keep one stable database snapshot, and multiple active statements
share that snapshot exactly as pinned SQLite does.

Add an idempotent pager recovery operation:

```cpp
[[nodiscard]] Status ReadPager::CleanupReadState();
```

It:

- succeeds without work when no shared read lock is held;
- releases a retained shared lock after failed `BeginRead()`;
- retries unlock and clears snapshot state after failed `EndRead()`;
- rejects cleanup while pages remain pinned; and
- preserves the current lock and snapshot state when unlock fails again.

Strict `BeginRead()` and `EndRead()` contracts remain unchanged.

The bookkeeping count is decremented even if `EndRead()` fails. The pager
then remains active with no logical owner, which is a pending cleanup state.
Whenever the logical count is zero, the state calls `CleanupReadState()` as
an execution barrier before every substantive `Prepare()` and every
`Step()`, including snapshot-free statements. This also drains a shared
lock retained by failed begin cleanup. No operation may execute past a failed
cleanup barrier.

Automatic step-after-DONE or step-after-ERROR discards the prior reset result
only after this cleanup barrier succeeds. A fresh cleanup failure is returned
and the retained prior execution error remains available to an explicit
`Reset()` or later retry.

The API is externally serialized. Concurrent calls through handles sharing
one state are unsupported and require caller synchronization.

### Schema-aware bytecode requirement

Not every program should start a read transaction. Pinned SQLite executes a
constant statement such as `SELECT 1` without entering a database
transaction. Cursor count alone cannot express this rule because a
table-dependent plan optimized to an empty result may contain no cursor but
must still observe schema changes.

Extend `ProgramInput` and `BytecodeProgram` with:

```cpp
bool requires_database_snapshot = false;
```

Add:

```cpp
[[nodiscard]] bool requires_database_snapshot() const noexcept;
```

`ProgramBuilder::AddCursor()` sets the requirement automatically.
`ProgramBuilder::RequireDatabaseSnapshot()` sets it explicitly without
allocation. Lowering calls `RequireDatabaseSnapshot()` whenever the bound
SELECT has a table source, including an Empty access path.

`BytecodeProgram::Create()` canonicalizes its owned input before verification
and publication:

```cpp
canonical.requires_database_snapshot =
    input.requires_database_snapshot ||
    input.transaction_access == ProgramTransactionAccess::kWrite ||
    !input.cursors.empty() ||
    !input.write_cursors.empty();
```

The builder applies the same effective rule. The verifier validates the
canonical input, and `requires_database_snapshot()` returns the canonical
value. A directly constructed cursor program therefore cannot publish a
false requirement.

`Vm::AttachExecutionContext()` and the first step validate the complete schema
cookie, catalog generation, database encoding, and schema format when a
transaction is active. When no transaction is active, attachment may execute
only a program that does not require one; in that case it validates the
retained catalog generation.

Suspending and resuming a snapshot-free program does not inspect pager
transaction state or data version. A snapshot-requiring program retains
the existing strict snapshot and data-version checks.

### Automatic reprepare

Before starting an execution that requires a read transaction, the statement:

1. acquires or joins the session snapshot;
2. refreshes the catalog;
3. compares the program's schema cookie and generation with the current
   catalog; and
4. recompiles from retained SQL if either value differs.

The statement then attaches `VmExecutionContext`. DONE and ERROR detach before
releasing the shared read reference. Reset and finalization use the VM reset
path to close cursors and detach before ending the reference.

A snapshot-free statement also recompiles if another operation has
already published a newer catalog generation. It does not open a transaction
solely to discover an otherwise unobservable external schema change.

Reprepare uses the same compile pipeline as prepare and must produce the same
parameter count. Every current binding is cloned into the replacement VM by
index before publication. Any mismatch is an internal error.

If reprepare fails:

- the old execution object and bindings remain intact;
- the newly acquired transaction reference is released;
- the statement enters ERROR;
- `Step()` returns the compile or cleanup error; and
- `Reset()` can return the error and make the old statement ready for a
  later retry.

Result metadata belongs to the current bytecode program. It may therefore
change during `Step()` when automatic reprepare succeeds, matching SQLite's
`SELECT *` behavior after schema change.

### Statement states

The public state machine is:

```text
                Bind
                 |
                 v
              +-------+
     Reset -->| READY |<---------------------------+
              +---+---+                            |
                  | Step                           |
                  v                                |
              +---+---+   Step(row)   +--------+  |
              |  ROW  |-------------->|  ROW   |  |
              +---+---+               +--------+  |
                  | Step(done/error)               |
                  v                                |
        +---------+---------+                      |
        |                   |                      |
     +--+---+            +--+---+                  |
     | DONE |            | ERROR|                  |
     +--+---+            +--+---+                  |
        | Step              | Step                 |
        +--- automatic reset+----------------------+

READY, ROW, DONE, ERROR -- Finalize/destructor --> FINALIZED
ROW, DONE, ERROR ------- Reset ------------------> READY
```

`Step()` from DONE or ERROR first performs the equivalent of `Reset()`,
discards the previous reset return value as part of SQLite's automatic-reset
contract, and starts a new execution. `Step()` from ROW continues the current
execution.

`Reset()`:

- closes all VM cursors;
- releases any transaction reference;
- clears registers and the current row;
- preserves parameter bindings;
- returns the prior execution error when called from ERROR; and
- leaves the statement READY even when returning that prior error, unless
  transaction cleanup itself remains unresolved.

`Finalize()` performs reset-style cleanup, destroys the execution object and
retained SQL, and leaves the statement invalid. It returns the prior
execution error, or a cleanup error when no execution error exists.
`Finalize()` on an already finalized or moved-from statement succeeds, which
makes explicit cleanup safely idempotent in C++.

The destructor calls `Finalize()` as a best-effort RAII fallback and cannot
report its status.

### Bindings and parameter metadata

The public parameter index is one-based. Index zero or an index greater than
`parameter_count()` returns `ErrorCode::kOutOfRange`.

`Bind()` is valid only in READY. It delegates value-size enforcement and
cloning to `Vm::Bind()`. Binding while ROW, DONE, or ERROR returns
`ErrorCode::kMisuse`.

Every parameter is initially NULL. Reset and automatic reprepare preserve
bindings. `ClearBindings()` is intentionally deferred because SQLite permits
it while a statement is active but the current VM permits mutation only in
READY; adding it now would expand the lifecycle contract without being part
of this node's deliverable.

The statement copies the binder's parameter-name vector before destroying
the physical plan:

- `parameter_count()` is available in every non-finalized state;
- `parameter_name()` accepts a one-based index and returns the original
  first spelling or `std::nullopt` for an unnamed or invalid slot; and
- `parameter_index()` performs exact byte matching and returns zero when no
  name matches.

Thus `?0001` and `?1` may address the same slot while only the retained first
spelling resolves by name, matching pinned SQLite.

A view returned by `parameter_name()` survives binding, stepping, reset,
automatic reprepare, and move construction because the retained SQL defines
the same parameter vector. Any `Finalize()` call that leaves the statement
finalized invalidates the view regardless of whether finalization returns a
prior execution or cleanup error. Move assignment and destruction also
invalidate it. A finalized or moved-from statement returns `std::nullopt`.

Add an allocation-free VM accessor:

```cpp
[[nodiscard]] std::span<const SqlValue> bindings() const noexcept;
```

It exists so automatic reprepare can preserve bindings without a duplicate
session-owned value array.

### Result metadata and row lifetime

`result_columns()` exposes the immutable metadata owned by the current
program. The span is valid until the next non-const statement operation,
automatic reprepare, finalize, move assignment, or destruction.

`row()` is nonempty only in ROW and borrows the VM register range. It remains
valid until the next `Step()`, `Reset()`, `Finalize()`, move assignment, or
destruction. Access is allocation-free.

`Step()` returns only `ReadStep::kRow` or `ReadStep::kDone`; errors use
`Result`. The row values are accessed separately to prevent a DONE result
from being confused with a valid zero-column shape.

### Error conversion and precedence

Session APIs return the existing `Error` type and preserve lower-layer
`ErrorCode` values.

Compilation errors map as follows:

- parser resource and depth limits -> `ErrorCode::kTooLarge`;
- parser internal invariant -> `ErrorCode::kInternal`;
- other parser errors -> `ErrorCode::kGeneric`;
- binder, planner, optimizer, lowering, program, VM, catalog, pager, and VFS
  errors -> their published base error code; and
- `std::bad_alloc` at any session orchestration boundary ->
  `Error::OutOfMemory()`.

Messages include the failing phase, stable lower-layer error name, available
detail, and source byte offset for syntax or binding errors. Exact English
wording is diagnostic, not a compatibility surface; error codes and state
changes are.

When a session operation has already produced a primary compile or execution
error and the session's subsequent `EndRead()` or `CleanupReadState()` also
fails:

1. preserve the primary operation's error code;
2. append the cleanup error code and message to its diagnostic; and
3. retain that combined error for reset/finalize behavior.

When cleanup is the only failure, return the cleanup error directly.
Destructors ignore returned cleanup errors but leave shared state able to
retry pending pager cleanup while another handle exists.

`ReadPager::BeginRead()` retains its own published precedence: if snapshot
setup fails and its immediate unlock also fails, the pager's returned cleanup
error is the primary error seen by the session. The session does not attempt
to reconstruct a hidden setup error.

### Allocation and exception guarantees

The public boundary catches `std::bad_alloc` and returns
`Error::OutOfMemory()`.

Open provides the strong guarantee: failure publishes no session.

Catalog refresh is an independent commit point. Once a complete immutable
catalog is loaded successfully, the state may publish its new snapshot and
generation even if later binding, planning, lowering, VM creation, binding
restoration, or temporary transaction cleanup fails. The published catalog
is valid state, not a partial statement.

Prepare publishes no statement until compilation and required cleanup
succeed. Reprepare preserves the old execution object, metadata, and bindings
until the complete replacement is ready. These statement-level operations
provide the strong guarantee relative to their own published objects, not a
rollback of an independently successful catalog refresh.

Successful `Reset()`, `Finalize()` after prior explicit cleanup, metadata
queries, parameter lookup, and row access allocate nothing. Session
orchestration adds no allocation to a normal `Step()`; allocations required
by VM value cloning or record decoding remain governed by the VM contract.
Schema-stable reset-and-step loops do not reparse, rebind, replan, relower,
or rebuild a VM.

The implementation must not add broad catches, silent cleanup success, or
success-shaped fallbacks.

### TDD and verification

Implementation begins only after this ADR is independently reviewed and
accepted.

Red-first tests must compile the public contract and initially fail only on
missing session symbols. The test matrix covers:

- missing, corrupt, empty, and valid database open/prepare timing;
- first-statement tails and trivia-only input;
- constant, full-scan, rowid, filter, projection, LIMIT/OFFSET, and
  parameterized execution against pinned fixtures;
- one-based binding, NULL defaults, binding persistence, and bind misuse;
- parameter names including `?0001`, `?1`, named repetition, and anonymous
  slots;
- row and metadata lifetime;
- automatic reset after DONE and ERROR;
- reset/finalize prior-error propagation;
- two simultaneously active statements sharing one snapshot;
- prepare while another statement is suspended;
- schema-cookie refresh, automatic reprepare, binding preservation, and
  `SELECT *` metadata change;
- statements outliving the public session handle;
- move construction, move assignment, explicit finalize, and destructor
  cleanup;
- failed begin/end cleanup with deterministic VFS doubles;
- catalog-generation overflow through a checked internal helper whose current
  value can be seeded directly by its focused unit test;
- every session-owned allocation boundary;
- no session dependency from parser, catalog, bytecode, VM, or lower layers;
  and
- the relaxed VM rule for snapshot-free versus snapshot-requiring
  programs.

Pinned SQLite comparison uses SQLite 3.54.0 and an immutable 1,000-row fixture
generated by the pinned shell. Performance evidence reports:

- cold open + prepare + first step + finalize;
- warm prepare with a cached catalog;
- prepared constant step + reset;
- prepared rowid bind + step-to-DONE + reset; and
- prepared full-scan step-to-DONE + reset.

The benchmark reports latency, ratios, and Modern SQLite allocation counts.
The existing 10x severe-regression gate applies to every comparable
workload. Any ratio above the gate blocks acceptance unless a narrower
documented SQLite-compatibility requirement makes the cost unavoidable.

The reviewed implementation's final Apple Clang 21 arm64 benchmark produced:

| Workload | Unit | Modern median | SQLite median | Ratio | Modern allocations/unit |
|---|---|---:|---:|---:|---:|
| Cold open + prepare + first step + finalize | operation | 41361.660 ns | 42510.840 ns | 0.972967 | 134 |
| Warm prepare | operation | 5310.205 ns | 1070.415 ns | 4.960884 | 68 |
| Prepared constant step + reset | operation | 37.833 ns | 45.375 ns | 0.833785 | 0 |
| Prepared rowid bind + step-to-DONE + reset | operation | 2916.916 ns | 2453.416 ns | 1.188920 | 3 |
| Prepared full scan + reset | row | 139.967 ns | 51.471 ns | 2.719338 | 0.003 |

The full scan processes 1,000 rows and performs three Modern SQLite
allocations per execution, reported above as 0.003 allocations per row. The
maximum ratio is 4.960884, so every workload passes the 10x
severe-regression gate. Timing runs disable allocation instrumentation;
allocation counts come from a separate pass over the same public-API
workload. Raw samples, fixture generation, source hashes, compiler flags, and
provenance are stored in the session artifacts
`read_session_benchmark.cpp`, `read_session_benchmark-results.json`,
`read_session_benchmark_fixture.sql`, and
`read_session_benchmark-provenance.json`.

Validation includes focused tests, the full Debug and Release suites,
ASan/UBSan, TSan, clang-tidy, formatting, graph validation, layering tests,
and whitespace checks.

## Rejected alternatives

### Borrow the session from each statement

Rejected because moving or destroying the public session handle would
silently dangle prepared statements or require a fragile parent-before-child
rule. Shared zombie ownership is safer and matches `sqlite3_close_v2()`.

### Keep the bytecode program inline beside a movable VM

Rejected because moving the statement could change the program address while
the VM retains a pointer to it. One heap-stable execution object makes the
borrow explicit and mechanically safe.

### Open one pager per statement

Rejected because statements would not share one connection snapshot,
catalog, cache, or transaction lifetime. It also adds file descriptors,
locking work, and inconsistent schema generations.

### Hold a read transaction for the entire session

Rejected because prepare alone must not retain a transaction, writers would
be blocked unnecessarily, and constant statements should not enter a read
transaction.

### Use cursor presence as the transaction requirement

Rejected because an Empty plan derived from a table can contain no cursor
while remaining schema-dependent. The requirement must be explicit bytecode
metadata.

### Require explicit reset after DONE or ERROR

Rejected for this compatibility layer because pinned SQLite automatically
resets on the next step. The C++ API keeps explicit `Reset()` for error
observation and deterministic lifecycle control while preserving compatible
step behavior.

### Add clear-bindings now

Rejected because the current VM lifecycle conflicts with SQLite's
active-statement behavior and the dependency-graph deliverable does not
require it. Deferring avoids an unrelated VM mutation design.

### Expose every lower-layer options structure

Rejected as premature. It would leak generation fields and create an
unstable cross-module configuration aggregate before any application has
demonstrated the need.

### Cache syntax trees, plans, or prepared statements

Rejected because retained SQL is sufficient for rare schema reprepare.
Caching intermediate objects expands lifetime, memory, and invalidation
complexity without improving schema-stable execution.

## Consequences

- Applications gain one coherent read-only API without depending on parser,
  catalog, planner, lowering, bytecode, pager, or VM choreography.
- Session and statement ownership is safe across moves and public session
  destruction.
- Multiple statements share one snapshot and one catalog generation.
- Automatic schema reprepare preserves bindings and updates result metadata.
- Constant statements avoid unnecessary pager transactions.
- Empty databases and lazy corruption detection match pinned SQLite.
- Reset and finalize expose execution errors instead of losing them in RAII
  cleanup.
- The bytecode gains one explicit execution requirement, and the VM gains
  one read-only bindings accessor plus a narrow snapshot-free path.
- Clear-bindings, custom registries, connection options, statement caching,
  and concurrency remain future decisions rather than hidden scope in this
  node.
