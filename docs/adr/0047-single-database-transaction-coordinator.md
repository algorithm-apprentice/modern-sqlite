# ADR-0047: Reference-Faithful Single-Database Transaction Coordination

- Status: Accepted
- Date: 2026-10-07

## Context

ADR-0040 through ADR-0042 provide SQLite-compatible rollback-journal,
savepoint, pager, spill, commit, rollback, and hot-recovery mechanisms.
ADR-0044 provides the writable B-tree and explicitly assigns statement
atomicity to the next DAG node.

The `implement-transaction-coordinator` node must now compose those mechanisms
into one single-database, single-connection transaction owner. Its
machine-readable deliverable is:

> Single-database implicit/explicit transactions, statement rollback, and
> savepoints.

The lower layers intentionally do not own connection policy:

- `Pager` knows read and write transaction mechanics but not autocommit,
  explicit `BEGIN`, named savepoints, or statement success.
- `JournalTransaction` knows opaque nested savepoint identities but not SQL
  names or transaction-savepoint semantics.
- `BtreeWriteSession` owns one write-generation mutation authority but not
  statement boundaries.
- `ReadVm` deliberately does not begin or end transactions.
- `ReadSession` is read-only and ends an implicit read transaction when its
  final active read statement finishes.

Pinned SQLite 3.54.0 provides the reference mechanism:

- `src/build.c:5323-5354` compiles deferred `BEGIN` as an autocommit-state
  change, while immediate and exclusive forms first request a B-tree
  transaction.
- `src/vdbe.c:3959-4145` stores named savepoints newest-first, permits
  duplicate names, resolves names with `sqlite3StrICmp()`, makes a savepoint
  opened from autocommit a transaction savepoint, releases the matched
  savepoint and every newer savepoint, and retains the matched savepoint after
  `ROLLBACK TO`.
- `src/vdbe.c:4147-4208` rejects nested `BEGIN`, rejects commit or rollback
  without an active transaction, and keeps a failed commit retryable.
- `src/vdbe.c:4210-4310` upgrades a transaction when a write statement starts
  and opens an anonymous statement savepoint when statement rollback is
  required.
- `src/btree.c:4615-4635` implements a statement transaction as a pager
  savepoint newer than every named savepoint.
- `src/btree.c:4637-4675` rolls back or releases a savepoint while retaining
  the outer write transaction.
- `src/vdbeaux.c:3220-3280` rolls a failed statement back to its anonymous
  savepoint and then releases that savepoint.
- `src/vdbeaux.c:3320-3510` commits an autocommit statement, rolls back the
  complete implicit transaction on failure, or closes only the statement
  savepoint inside an explicit transaction.

The first Modern SQLite transaction coordinator must reproduce those
mechanisms without importing SQLite's connection-wide mutable `sqlite3`,
`Vdbe`, attached-database array, virtual-table callbacks, constraint
counters, or source-level dependency cycles.

This node is performance-sensitive, but correctness and durable ordering take
priority. The first implementation uses the existing pager and B-tree
mechanisms directly. It does not add a second journal, duplicate page
membership, hidden rollback in destructors, speculative lock retry, or a
parallel transaction state machine.

## Decision

### 1. Add a transaction module that owns one writable pager

Add:

- `include/modern_sqlite/transaction/transaction_coordinator.hpp`;
- `src/transaction/transaction_coordinator.cpp`; and
- focused tests under `tests/unit/transaction`.

`TransactionCoordinator` takes unique ownership of one writable `Pager`.
Transferring the pager, rather than borrowing it, makes the coordinator the
only ordinary owner allowed to begin, commit, roll back, or create pager
savepoints. Future writable session state owns its VFS before the coordinator,
so the VFS still outlives the pager.

Opening a coordinator requires:

- a non-null pager;
- a writable pager;
- `PagerState::kOpen`; and
- no active read or write transaction.

The existing read-only `ReadSession` remains unchanged. Node 38 will compose
the parser, compiler, VM, catalog, and this transaction coordinator into a
writable session. This node does not parse transaction SQL.

### 2. Expose typed transaction and statement scopes

The public transaction contract is:

```cpp
enum class TransactionMode : std::uint8_t {
  kDeferred,
  kImmediate,
};

enum class TransactionState : std::uint8_t {
  kAutocommit,
  kExplicit,
  kSavepoint,
};

enum class StatementAccess : std::uint8_t {
  kRead,
  kWrite,
};

enum class StatementRollbackMode : std::uint8_t {
  kTransaction,
  kStatement,
};

struct TransactionStatementOptions {
  StatementAccess access = StatementAccess::kRead;
  StatementRollbackMode rollback = StatementRollbackMode::kStatement;
};

class TransactionWriter;

class TransactionStatement final {
 public:
  TransactionStatement(const TransactionStatement&) = delete;
  TransactionStatement& operator=(const TransactionStatement&) = delete;
  TransactionStatement(TransactionStatement&&) noexcept;
  TransactionStatement& operator=(TransactionStatement&&) noexcept;
  ~TransactionStatement();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] StatementAccess access() const noexcept;
  [[nodiscard]] TransactionWriter* writer() noexcept;

  [[nodiscard]] Status Succeed();
  [[nodiscard]] Status Rollback();

 private:
  friend class TransactionCoordinator;
};

class TransactionWriter final {
 public:
  TransactionWriter(const TransactionWriter&) = delete;
  TransactionWriter& operator=(const TransactionWriter&) = delete;
  TransactionWriter(TransactionWriter&&) = delete;
  TransactionWriter& operator=(TransactionWriter&&) = delete;
  ~TransactionWriter() = default;

  [[nodiscard]] Status InitializeDatabase(
      BtreeDatabaseOptions options = {});
  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree();
  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(
      std::span<const IndexColumnOrder> columns);
  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(
      PageNumber root_page);
  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(
      PageNumber root_page,
      std::span<const IndexColumnOrder> columns);

 private:
  friend class TransactionStatement;
};

class TransactionCoordinator final {
 public:
  [[nodiscard]] static Result<TransactionCoordinator> Open(
      std::unique_ptr<Pager> pager);

  TransactionCoordinator(const TransactionCoordinator&) = delete;
  TransactionCoordinator& operator=(const TransactionCoordinator&) = delete;
  TransactionCoordinator(TransactionCoordinator&&) noexcept;
  TransactionCoordinator& operator=(TransactionCoordinator&&) noexcept;
  ~TransactionCoordinator();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool autocommit() const noexcept;
  [[nodiscard]] TransactionState state() const noexcept;
  [[nodiscard]] bool statement_active() const noexcept;

  [[nodiscard]] Status Begin(
      TransactionMode mode = TransactionMode::kDeferred);
  [[nodiscard]] Status Commit();
  [[nodiscard]] Status Rollback();

  [[nodiscard]] Status Savepoint(Utf8View name);
  [[nodiscard]] Status Release(Utf8View name);
  [[nodiscard]] Status RollbackTo(Utf8View name);

  [[nodiscard]] Result<TransactionStatement> BeginStatement(
      TransactionStatementOptions options = {});

 private:
  struct State;
};
```

`writer()` is a borrowed pointer to a non-movable capability owned by the
statement implementation. It is null after the statement becomes invalid and
for a read statement. The capability exposes only typed B-tree operations. It
cannot commit, roll back, end the read transaction, manipulate savepoints, or
move the coordinator-owned `BtreeWriteSession`.

The unrestricted `Pager` and owning `BtreeWriteSession` remain private to
coordinator state. Node 38 may use a private friend bridge to construct a VM
environment from the active statement; no unrestricted pager accessor becomes
part of this public contract.

`Succeed()` and `Rollback()` retain the statement only when the failed
lower-layer operation is itself retryable. A rollback-required pager failure
transitions through the full-rollback policy in Decision 9 instead of leaving
a success-shaped statement scope.

Destructors perform no commit, rollback, savepoint operation, allocation, or
I/O. Destroying an unfinished statement leaves the coordinator's active
statement token in place. The connection may then perform a full
`Rollback()`, but cannot silently commit or begin another statement.

### 3. Model SQLite autocommit and transaction-savepoint state

The coordinator starts in `kAutocommit`.

`Begin(kDeferred)`:

- changes only coordinator state to `kExplicit`;
- performs no pager I/O or lock acquisition; and
- rejects any already active explicit or savepoint transaction.

This mirrors SQLite's deferred `BEGIN`.

`Begin(kImmediate)`:

1. begins a pager read transaction;
2. upgrades it to a pager write transaction; and
3. publishes `kExplicit` only after both operations succeed.

If the write upgrade fails after the read begins, the coordinator attempts
`EndRead()` and returns the first setup error, augmented with cleanup failure
when necessary.

Rollback-mode `BEGIN EXCLUSIVE` is deferred to the multi-connection locking
node because the current pager intentionally acquires RESERVED at
`BeginWrite()` and EXCLUSIVE only before database modification. Treating that
as exclusive now would claim an observable lock guarantee the implementation
does not provide.

`Savepoint(name)` while in autocommit creates a transaction savepoint:

- state becomes `kSavepoint`;
- autocommit becomes false;
- the savepoint is the outermost named scope; and
- releasing that outermost scope commits the transaction.

`Commit()` and full `Rollback()` are valid in either `kExplicit` or
`kSavepoint`. Both clear every named savepoint only after the terminal pager
transition and read-lock cleanup succeed.

Commit or nested `Begin()` in ordinary `kAutocommit` returns `kMisuse`.
`Rollback()` also returns `kMisuse` when no statement, pager transaction, or
terminal cleanup is active. It remains legal in `kAutocommit` when recovering
an active or abandoned implicit statement:

- an implicit read invalidates the statement and completes `EndRead()`; and
- an implicit write performs full pager rollback and then read cleanup.

### 4. Start pager transactions lazily for deferred scopes

A deferred explicit transaction or logical savepoint stack may exist while
the pager remains `kOpen`.

The first read statement:

- calls `BeginRead()`; and
- retains that snapshot after statement completion while autocommit is off.

The first write statement:

1. calls `BeginRead()` when no snapshot exists;
2. calls `BeginWrite()` when no pager write transaction exists;
3. materializes every active named savepoint from oldest to newest through
   `Pager::CreateSavepoint()`; and
4. opens the transaction-owned `BtreeWriteSession`.

Logical savepoints created before the first write all describe the same
unmodified database image. Materializing them when the write transaction
starts therefore reproduces the same rollback points without opening an
otherwise unnecessary rollback journal.

If named-savepoint materialization fails partway through, the journal
transaction is rollback-required. The coordinator:

1. preserves that first failure;
2. fully rolls back the pager write transaction;
3. ends the retained read transaction;
4. clears every pager identity and logical savepoint; and
5. returns to autocommit only after cleanup succeeds.

It does not retain an apparently usable explicit transaction whose physical
savepoint stack was only partly published.

### 5. Give every statement one explicit coordinator token

Only one transaction statement may be active at a time in this node.
Attempting to begin a second statement returns `kBusy`.

This is a deliberate initial boundary, not a connection-concurrency
mechanism. The existing read-only session continues to support overlapping
read statements. Node 38 may broaden the writable-session statement
scheduler after its VM ownership is concrete. Node 37 first establishes the
atomic write path without inventing cursor suspension policy.

An active statement token blocks:

- `Begin()`;
- `Commit()`;
- named `Savepoint()`;
- `Release()`; and
- `RollbackTo()`.

Full `Rollback()` remains legal. It invalidates the active statement token
and every B-tree handle through the pager write-generation transition. This
is the recovery path for an abandoned statement object.

Each statement captures a monotonically increasing coordinator token.
Operations through a stale statement return `kSchemaChanged` and perform no
pager or B-tree work.

### 6. Use implicit transactions and anonymous savepoints like SQLite

For a read statement in autocommit:

- `BeginStatement()` starts a read transaction;
- both `Succeed()` and `Rollback()` end that read transaction; and
- no journal or B-tree writer is created.

For a write statement in autocommit:

- `BeginStatement()` starts one pager read/write transaction and opens the
  transaction-owned B-tree writer;
- `Succeed()` commits the complete pager transaction, then ends the retained
  read transaction; and
- `Rollback()` fully rolls back the pager transaction, then ends the retained
  read transaction.

For a write statement inside an explicit or savepoint transaction:

- `StatementRollbackMode::kStatement` creates one anonymous pager savepoint
  newer than every named savepoint before exposing the writer;
- `Succeed()` releases that anonymous savepoint;
- `Rollback()` rolls back to it and then releases it; and
- the outer transaction remains active.

This is the Modern equivalent of `sqlite3BtreeBeginStmt()` and
`vdbeCloseStatement()`.

`StatementRollbackMode::kTransaction` creates no anonymous savepoint.
Rolling back such a failed write statement rolls back the complete outer
transaction. This corresponds to a statement for which SQLite does not set
`usesStmtJournal`, or to an error class that requires transaction rollback.

A read statement rejects `StatementRollbackMode::kTransaction` as
`kMisuse`; the option is meaningful only for writes.

Node 38 initially selects statement rollback conservatively for write
programs that may change more than one row or raise an ABORT-style error.
Node 40 may measure whether narrower admission materially affects performance.

### 7. Own one B-tree write session per pager write generation

The coordinator owns at most one `BtreeWriteSession`.

- It opens the session lazily for the first write statement.
- It opens that session in coordinator-managed mode and begins one monotonic
  B-tree statement epoch before exposing it.
- Successful statement-savepoint release retains the session for the outer
  transaction.
- Every successful or rolled-back statement ends the current statement epoch.
- Full commit or rollback destroys the session before invoking the terminal
  pager operation.
- Statement or named savepoint rollback destroys the session before
  `RollbackToSavepoint()`, because the pager advances its write generation.
- A later write statement opens one new session in the restored generation.

The statement's `TransactionWriter` forwards to that private session after
validating the statement token. Typed table/index handles created through the
capability capture both the pager write generation and the managed B-tree
statement epoch. Ending the statement advances the epoch without reopening
the B-tree session. An escaped handle therefore returns `kSchemaChanged`
after either successful release or rollback and cannot mutate outside its
statement. Savepoint and full rollback additionally invalidate handles
through the existing pager generation checks.

Direct Node 36 `BtreeWriteSession::Open()` remains unmanaged for its low-level
storage tests and callers. A private transaction-coordinator construction
path enables managed epochs; it does not add a second public writer mode or a
plugin callback.

The coordinator never opens a second B-tree session merely because one
wrapper was destroyed. It follows ADR-0044's one-claim-per-write-generation
rule.

### 8. Store named savepoints in SQLite stack order

The coordinator owns a vector ordered oldest to newest. Each entry contains:

```cpp
struct NamedSavepoint {
  std::string name;
  std::optional<JournalSavepointId> pager_id;
};
```

The vector is the Modern ownership equivalent of SQLite's newest-first linked
list.

Names:

- are validated as UTF-8 before state changes;
- may be empty;
- may be duplicated; and
- compare with SQLite ASCII case folding through `SqliteToLower()`.

Lookup scans newest to oldest and selects the first matching name.

For an ordinary `Release(name)`:

- the coordinator releases the matched pager savepoint when materialized; and
- only after that succeeds, removes the match and every newer logical
  savepoint.

Releasing the outermost transaction savepoint is a distinct terminal path.
The coordinator does **not** call `Pager::ReleaseSavepoint()` first. It keeps
the complete logical and pager savepoint stack, attempts whole-transaction
commit, and removes the stack only after commit and read cleanup succeed.
A retryable commit failure therefore preserves the transaction savepoint for
retry or rollback, matching SQLite's `OP_Savepoint` path.

`RollbackTo(name)`:

- destroys the current B-tree write session;
- rolls the pager back to the matched savepoint when materialized;
- removes every newer logical savepoint;
- retains the matched savepoint and its name; and
- leaves the outer transaction active.

Unknown names return `kGeneric`, matching SQLite's ordinary
`no such savepoint` error category, without changing state.

Name ownership and vector capacity are prepared before creating a pager
savepoint. Once `Pager::CreateSavepoint()` succeeds, publishing the logical
entry performs no allocation.

### 9. Preserve exact lower-layer failure and retry semantics

The coordinator does not convert lower-layer errors into success-shaped
states.

- Pre-I/O validation failures and `kBusy` leave coordinator state unchanged
  and may be retried.
- Pager or B-tree typed errors propagate with their original code.
- A B-tree `requires_rollback()` condition is resolved only through rollback
  to the statement's preexisting anonymous savepoint or full transaction
  rollback.
- Successful rollback to a savepoint clears the stale B-tree session and
  admits one new session in the restored generation.
- Journal finalization and persistent pager errors remain persistent.

Pager savepoint backend failures are not generally retryable.
`JournalTransaction` enters rollback-required state after a failed create,
release, or rollback-to operation. Therefore:

- failed named-savepoint creation, release, or rollback-to triggers full pager
  rollback and read cleanup;
- failed anonymous statement-savepoint release or rollback triggers the same
  full rollback;
- the coordinator clears the explicit transaction, statement token, named
  stack, and B-tree session only after that full rollback succeeds; and
- it returns the original savepoint error, augmented with full-rollback or
  read-cleanup failure context when needed.

The coordinator never retains named scopes whose corresponding pager
savepoints have entered a failed or partially published state.

Commit handling distinguishes implicit and caller-owned transactions:

- a failed implicit write commit automatically attempts full rollback and
  read cleanup, matching SQLite statement halt behavior;
- if the pager is already in `kWriterFinished`, the logical commit is durable
  and only terminal commit cleanup is retried;
- a retryable explicit commit failure retains the explicit transaction and
  complete named stack, but admits only pager-legal commit, full rollback, or
  rollback-to-preexisting-savepoint operations;
- an explicit transaction whose pager reports rollback-required state admits
  rollback operations only; and
- release of an outer transaction savepoint follows the same explicit commit
  rules without first releasing that savepoint.

Full rollback is the final recovery boundary. Failure during full rollback or
journal finalization preserves the pager's persistent error or retry state and
does not publish autocommit success.

When pager commit or rollback succeeds but `EndRead()` fails, the durable
transaction result is not reversed. The coordinator records a terminal
cleanup phase. Repeating the same operation retries only `EndRead()`;
opposite terminal operations and new statements are rejected.

If an operation has both a primary failure and a cleanup failure, the returned
error retains the primary SQLite code and includes cleanup context, following
the read-session precedent.

### 10. Keep allocation and destructor behavior explicit

The coordinator state is allocated once at `Open()`. Statement construction
itself performs no heap allocation after any required name/savepoint or
B-tree setup succeeds.

Potential allocations are:

- coordinator shared-state creation;
- named-savepoint string ownership and vector growth;
- pager savepoint metadata;
- first B-tree write-session scratch; and
- lower-layer journal/cache setup.

Savepoint lookup, transaction-state queries, statement token validation, and
successful terminal state publication allocate nothing.

Every public allocation boundary catches `std::bad_alloc` and
`std::length_error` and returns `kOutOfMemory`. Allocation is completed before
the first pager or logical state change whenever possible.

Destructors perform no hidden I/O. Abandoning an active transaction may leave
a rollback journal for the next writable open to recover. Higher session
layers are responsible for explicit statement finalization and connection
rollback policy.

## Test Strategy

Implementation proceeds red-first.

### Ownership and state

- null, read-only, and non-open pager rejection;
- move-only coordinator and statement values;
- deferred begin performs no VFS or lock operation;
- immediate begin acquires the pager write transaction;
- nested begin and terminal operation without a transaction;
- one active statement and stale statement tokens;
- abandoned statement blocks new work but permits full rollback; and
- destructor paths perform no VFS mutation.

### Implicit and explicit statements

- implicit read success and error end the read transaction;
- explicit read retains one snapshot through later statements;
- implicit write success commits and failure fully rolls back;
- explicit write success retains the outer transaction;
- anonymous statement rollback restores only the failed statement;
- transaction-mode statement failure rolls back the complete transaction;
- B-tree post-mutation OOM/I/O failure followed by successful statement
  rollback and a later successful statement;
- successful savepoint rollback admits exactly one new B-tree writer session;
  and
- escaped handles become stale after statement or full rollback.

### Named savepoints

- creation before and after pager write start;
- oldest-to-newest materialization on write upgrade;
- duplicate and ASCII-case-insensitive names;
- newest matching release and rollback-to;
- release removes the match and newer scopes;
- rollback-to retains the match and removes newer scopes;
- savepoint opened from autocommit acts as a transaction savepoint;
- release of the transaction savepoint commits;
- full commit and rollback clear all names;
- unknown-name and active-statement errors preserve the stack; and
- OOM before pager publication preserves the stack, while lower savepoint
  publication failure performs full rollback to a valid autocommit state.

### Failure and cleanup

- read begin, write begin, B-tree session open, savepoint create, release,
  rollback-to, commit, rollback, and end-read failures;
- implicit commit failure followed by automatic full rollback;
- retryable explicit commit and transaction-savepoint release failure with
  the complete savepoint stack retained;
- rollback-required savepoint failure followed by automatic full rollback;
- rollback to a named savepoint after failed commit;
- failed terminal cleanup retries only the unfinished cleanup;
- primary plus cleanup error reporting;
- pager persistent-error propagation; and
- exhaustive allocation failure at every coordinator-owned growth boundary.

### Crash and interoperability

Deterministic cuts cover:

- implicit write commit;
- explicit transaction commit after multiple successful statements;
- statement rollback after a spilled B-tree mutation;
- named savepoint rollback followed by outer commit;
- release of a transaction savepoint; and
- full rollback after a coordinator-reported mutation failure.

Each recovered image must be exactly old or committed, reopen through Modern
SQLite and pinned SQLite 3.54.0, pass `PRAGMA integrity_check`, and match the
expected freelist and logical rows.

### Model and performance evidence

A deterministic state-machine model compares:

- coordinator autocommit state;
- pager read/write state;
- named savepoint stack;
- statement activity;
- table contents; and
- legal/error outcomes

against a small reference model across mixed begin, statement, savepoint,
release, rollback-to, commit, and rollback operations.

A fixed-work baseline records pager begins, savepoint operations, B-tree
writer-session opens, journal writes/syncs, database writes/syncs, and
allocations for implicit writes and explicit batched writes. It is diagnostic
evidence, not a SQLite timing claim. Matched write-performance comparison
remains Node 40.

The complete Debug and Release fast tiers, ASan/UBSan, clang-tidy, formatting,
project-graph validation, design review, and code review remain required.

## Consequences

### Positive

- Autocommit, explicit transactions, anonymous statement rollback, and named
  savepoints have one owner above Pager and B-tree.
- Deferred scopes remain cheap and do not open a journal until a write needs
  one.
- Statement failure can restore an explicit transaction without laundering a
  B-tree rollback-required state.
- Named savepoint behavior follows SQLite's duplicate-name and newest-match
  rules.
- Pager and B-tree generation invalidation remain the authority for stale
  mutable handles.
- The future writable session receives a typed transaction boundary without
  coupling Pager to parser, VM, or catalog types.

### Negative

- The first writable coordinator permits only one active statement.
- The transaction layer must retain cleanup state after a durable commit or
  rollback whose final read unlock fails.
- An unfinished statement or transaction is not automatically rolled back by
  a destructor.
- Conservative statement savepoints add work until Node 38 can classify
  statements more precisely and Node 40 measures the cost.

### Deferred

- transaction SQL parsing and lowering;
- DML/DDL execution and catalog refresh;
- overlapping writable-session statements and incremental blob handles;
- foreign-key, trigger, virtual-table, and deferred-constraint state;
- attached databases and super-journals;
- busy handlers and lock retry;
- true rollback-mode `BEGIN EXCLUSIVE`;
- multi-connection isolation and deadlock policy;
- WAL transactions, snapshots, and checkpoints; and
- public C API transaction compatibility.

## References

- `src/build.c:5323-5396`
- `src/vdbe.c:3959-4310`
- `src/vdbeaux.c:3220-3280`
- `src/vdbeaux.c:3320-3510`
- `src/btree.c:3835-3860`
- `src/btree.c:4327-4675`
- `src/pager.c:7057-7125`
- ADR-0032: Read-Only Bytecode Virtual Machine
- ADR-0037: RAII Read Session API
- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
