# ADR-0048: Writable SQL DML and CREATE TABLE Execution

- Status: Accepted
- Date: 2026-10-07

## Context

Nodes 1 through 37 provide:

- immutable syntax trees and a pure parser;
- immutable catalog snapshots loaded from `sqlite_schema`;
- SELECT binding, planning, lowering, typed bytecode, VM execution, and a
  read-only session;
- rollback-journal writable Pager and reference-faithful writable B-trees; and
- single-database implicit/explicit transactions, statement rollback, and
  named savepoints.

The `implement-dml-ddl` node must now deliver:

> Basic `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, and catalog mutation
> lowering.

Its acceptance criterion requires differential SQL traces to match rows,
types, errors, changes, and database contents.

The current parser intentionally rejects DML and transaction statements. Its
DDL syntax tree already represents `CREATE TABLE` and `CREATE INDEX` because
catalog loading needs to parse persisted schema SQL. The binder, plan,
lowering, bytecode, VM, and public session remain SELECT-only.

Pinned SQLite 3.54.0 establishes the reference mechanisms:

- `src/insert.c:888-1700` resolves target columns, applies affinity and
  constraints, selects or generates a rowid, builds records, and emits
  `OP_Insert`.
- `src/update.c:296-950` scans qualifying rows, computes replacement values,
  and performs delete/insert-style row replacement.
- `src/delete.c:288-760` scans qualifying rows and emits `OP_Delete`.
- `src/build.c:1265-1540,2716-2940` reserves and completes
  `sqlite_schema` rows, creates a table B-tree, updates schema metadata, and
  stores normalized CREATE SQL.
- `src/vdbe.c:3577-3725,5744-5995,6037-6135,7203-7250` implements record
  construction, rowid allocation, table insert/delete, and B-tree creation.
- `src/build.c:5323-5396` and `src/vdbe.c:3959-4310` map transaction SQL to
  connection autocommit and savepoint operations.

Modern SQLite must preserve observable SQL behavior while retaining its
acyclic architecture:

```text
syntax -> binder -> logical/physical plan -> lowering -> bytecode -> VM
                                                        |
                                                        v
                                              transaction capability
                                                        |
                                                        v
                                                   writable B-tree
```

Parser actions do not mutate the catalog or emit bytecode. The planner does
not write pages. The VM does not depend on AST or planner types. Catalog
mutation remains a typed storage operation executed inside a statement
transaction.

This node implements the smallest writable SQL surface needed for the
writable MVP. Index creation and index maintenance belong to Node 41.
Triggers, foreign keys, views, generated columns, advanced conflict actions,
and broader SQL belong to Node 42.

## Decision

### 1. Add a writable session without weakening the read-only API

Retain `ReadSession` and `ReadStatement` as explicit read-only APIs.

Add:

- `include/modern_sqlite/session/write_session.hpp`;
- `src/session/write_session.cpp`; and
- shared internal compilation helpers extracted from `read_session.cpp`
  where doing so removes real duplication.

`WriteSession` owns, in lifetime order:

1. one VFS;
2. one writable Pager through `TransactionCoordinator`;
3. one current immutable catalog snapshot;
4. catalog and schema generations;
5. connection change counters; and
6. prepared statement shared state.

The default path overload owns `PosixVfs`. A test/embedding overload accepts
`std::unique_ptr<Vfs>`.

`WriteSession::Open()` opens or creates the main database through
`Pager::OpenWritable()`. Opening a zero-byte file does not initialize SQLite
format until a write statement requires it.

`WriteStatement` preserves the existing prepare, bind, step, reset, finalize,
row-span, automatic reprepare, and zombie-session ownership rules. SELECT
statements continue to produce rows. DML, DDL, and transaction-control
statements produce `kDone`.

The first writable API additionally exposes:

```cpp
[[nodiscard]] std::uint64_t changes() const noexcept;
[[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
[[nodiscard]] bool autocommit() const noexcept;
```

`changes()` is updated only after successful DML statement completion.
Statement rollback reports zero changes. Full transaction rollback does not
rewind the most recently reported successful statement count, matching
SQLite connection semantics.

`last_insert_rowid()` has a different publication point. It changes when the
VM successfully performs the user-table INSERT operation and is not undone by
later statement, savepoint, or transaction rollback. Internal
`sqlite_schema` inserts and UPDATE replacement inserts do not change it.

`WriteSession` and prepared statements share zombie state as in the read-only
API. When the final shared writable-session state is destroyed, it first
destroys every VM/cursor resource and then performs a best-effort full
rollback whenever the coordinator is not in clean autocommit state or has an
abandoned statement token. It never commits from a destructor. A rollback
failure may leave a hot journal for the next writable open, but normal RAII
close does not intentionally behave like a crash.

### 2. Extend syntax trees with basic DML and transaction statements

Add immutable AST nodes for:

```text
INSERT INTO qualified-name [(column, ...)]
  VALUES (expression, ...)

INSERT INTO qualified-name DEFAULT VALUES

UPDATE qualified-name
  SET column = expression [, ...]
  [WHERE expression]

DELETE FROM qualified-name
  [WHERE expression]

BEGIN [DEFERRED|IMMEDIATE] [TRANSACTION]
COMMIT [TRANSACTION]
END [TRANSACTION]
ROLLBACK [TRANSACTION]
SAVEPOINT name
RELEASE [SAVEPOINT] name
ROLLBACK [TRANSACTION] TO [SAVEPOINT] name
```

The parser also continues to produce the existing `CreateTableStatement`.

The Node 38 parser explicitly rejects:

- multi-row VALUES lists;
- `INSERT ... SELECT`;
- `REPLACE`, `OR` conflict clauses, UPSERT, and RETURNING;
- UPDATE/DELETE ORDER BY, LIMIT, FROM, and RETURNING;
- common-table expressions;
- `BEGIN EXCLUSIVE`;
- qualified savepoint names; and
- every unsupported DML/transaction tail.

All lists obey existing source, column, expression-depth, and parser-depth
limits. Exact source spans and raw token text remain retained.

### 3. Bind mutations against one immutable catalog snapshot

Add bound statement variants:

- `BoundInsert`;
- `BoundUpdate`;
- `BoundDelete`;
- `BoundCreateTable`; and
- typed transaction-control statements.

The public contracts live in
`include/modern_sqlite/binder/bound_statement.hpp`. `BindStatement()`
returns one move-only `BoundStatement` variant while the existing
`BindSelectStatement()` API remains available to the read-only pipeline.

Binding resolves:

- the target main-database table;
- target column IDs and duplicate assignments;
- the rowid alias, if any;
- input expressions against parameters and, for UPDATE/DELETE, the current
  target row;
- column affinity, declared type, nullability, and constant default;
- the physical table root page; and
- the schema cookie and catalog generation required at execution.

Duplicate targets follow pinned SQLite rather than a newly invented
rejection rule:

- INSERT keeps the first value for a repeated ordinary stored column;
- INSERT keeps the last value for repeated hidden-rowid or INTEGER PRIMARY
  KEY aliases; and
- UPDATE keeps the last assignment to any repeated target.

All duplicate-source expressions still bind and retain their parameter slots.
An explicit INSERT column list combined with `DEFAULT VALUES` therefore
reports the same zero-values/column-count error as SQLite.

Node 38 accepts only ordinary rowid tables with no stored secondary or
automatic indexes. Mutation of a table with any index is rejected with
`kProtocol` before a transaction statement or page mutation begins. This is
required because Node 41 owns index creation and maintenance; silently
changing table rows without indexes would corrupt logical database contents.

Writable CREATE TABLE accepts:

- the main database only;
- a non-temporary ordinary rowid table;
- one or more columns;
- declared types supported by the current catalog model;
- at most one column declared exactly `INTEGER PRIMARY KEY`;
- column `NULL`/`NOT NULL` with default ABORT behavior; and
- constant NULL, numeric, text, or blob defaults representable by the current
  expression/value layer.

It rejects before mutation:

- `WITHOUT ROWID`, STRICT, TEMP, and CREATE TABLE AS SELECT;
- AUTOINCREMENT;
- UNIQUE, non-rowid PRIMARY KEY, CHECK, COLLATE, named constraints, and every
  table constraint;
- non-default conflict actions;
- generated columns and foreign keys; and
- CREATE INDEX execution.

The parser may continue to represent those forms for catalog loading. The
writable binder reports the unsupported execution boundary explicitly.

### 4. Add immutable mutation plans before lowering

Add logical and physical mutation plans rather than emitting bytecode from
binding.

The initial physical strategies are:

- CREATE TABLE catalog mutation;
- one-row VALUES insertion;
- rowid-table scan plus point delete; and
- rowid-table scan plus point replacement.

`LogicalMutationPlan` owns the move-only bound mutation and one typed logical
payload. `LogicalStatementPlan` is a variant of the existing SELECT
`LogicalPlan`, one `LogicalMutationPlan`, or a typed transaction-control
statement. Logical mutation payloads retain only statement semantics such as
the predicate ID, whether UPDATE may change rowid, and whether CREATE is an
`IF NOT EXISTS` no-op.

`PhysicalMutationPlan` owns the logical mutation and publishes:

- `kEmpty`, `kTableScan`, or `kRowIdLookup` access for UPDATE/DELETE;
- stable statement-guard and residual-predicate expression IDs;
- whether a scan UPDATE must collect qualifying original rowids before
  mutation; and
- transaction-atomic versus anonymous-statement atomicity.

INSERT is one-row transaction-atomic work. CREATE TABLE always uses
anonymous-statement atomicity when it mutates. Scan UPDATE/DELETE use
anonymous-statement atomicity; exact-rowid DELETE and same-rowid UPDATE use
transaction atomicity; and any UPDATE that can change rowid uses
anonymous-statement atomicity.

There is no cost-based write optimizer in this node. The optimizer validates
the supported shape and chooses:

- direct rowid lookup when the current predicate normalization proves one
  exact rowid equality; or
- a full rowid-table scan otherwise.

The physical plan remains inspectable and owns no Pager, cursor, statement,
or mutable catalog state.

### 5. Generalize bytecode and VM naming only where writes require it

Rename the execution-neutral types:

- `Vm` -> `Vm`;
- `VmEnvironment` -> `VmEnvironment`;
- `VmState` -> `VmState`; and
- `VmStep` -> `VmStep`.

Do not retain compatibility aliases; the project is not released and one
canonical API avoids permanent read/write naming debt.

`BytecodeProgram` gains:

- a statement kind and required transaction access;
- statement rollback mode;
- write-cursor descriptors;
- mutation result metadata; and
- typed mutation instructions.

The execution-neutral metadata is:

- `ProgramStatementKind`;
- `ProgramTransactionAccess`;
- `ProgramRollbackMode`;
- `requires_database_snapshot`;
- typed `ReadCursorDescriptor` and `WriteCursorDescriptor` arrays; and
- `MutationResultMetadata`.

Read cursor IDs and write cursor IDs are distinct strong types. Program
verification validates write roots, rowid-alias positions, column defaults,
NOT NULL metadata, and the consistency of statement kind, access, rollback,
result, and cursor metadata before publication.

The initial mutation instructions mirror SQLite mechanisms through Modern
types:

- open/close writable table capability;
- choose or validate rowid;
- build an encoded table record;
- insert/replace a row;
- delete a row;
- create a table root;
- insert the `sqlite_schema` row;
- increment the schema cookie; and
- report a VM-local mutation count and user-insert rowid event.

The first executable INSERT slice defines these concrete typed instructions:

- `OpenWriteCursorInstruction` and `CloseWriteCursorInstruction` acquire and
  release the table writer selected by a `WriteCursorDescriptor`;
- `ResolveInsertRowIdInstruction` consumes one initialized register, validates
  a non-NULL value losslessly as int64 or generates a rowid, and writes the
  resulting integer to its output register;
- `BuildTableRecordInstruction` consumes one contiguous initialized register
  block whose size exactly matches the descriptor's stored columns, applies
  affinity and NOT NULL checks to ordinary stored columns, stores NULL for the
  already-resolved rowid alias, and writes encoded record bytes to its output
  register; and
- `InsertTableInstruction` consumes the resolved rowid and encoded record,
  performs insert-only B-tree mutation, increments the VM-local change count,
  and publishes the user-insert rowid event when requested by program
  metadata.

Program verification requires every INSERT operand and contiguous register
range to be in bounds and initialized, and requires the referenced write
cursor to be open for rowid resolution, record construction, and insertion.

The executable DELETE slice extends the existing rowid seek and write
instruction set with:

- `RowIdSeekMode::kEqual` and `RowIdSeekMode::kGreater`, carried by
  `SeekRowIdInstruction`, so a closed-and-reopened scan can seek strictly
  beyond its last owned rowid without exposing B-tree cursor types to
  lowering; and
- `DeleteTableInstruction`, which consumes one initialized rowid register and
  one open write cursor, performs one point delete, and increments the
  VM-local change count only when a row was actually removed.

The verifier rejects invalid seek modes, requires rowid seeks to use a
rowid-table read cursor, and requires DELETE operands to reference an open
write cursor and an initialized register. The VM maps seek modes directly to
the B-tree's exact or strictly-greater seek, rejects a malformed non-integer
DELETE operand as `kTypeMismatch`, propagates storage errors, treats an
already-absent row as a successful no-op, and never publishes a
last-insert-rowid event for DELETE.

The executable UPDATE slice adds `UpdateTableInstruction`, which consumes an
open write cursor plus initialized old-rowid, new-rowid, and encoded-record
registers. The verifier enforces those operands and cursor lifetime.

At execution the VM:

1. converts both rowids losslessly to int64 and rejects NULL, fractional,
   malformed, or out-of-range values with `kTypeMismatch`;
2. point-checks that the old row still exists;
3. when the rowid changes, point-checks the new rowid and returns
   `kConstraint` before mutation if it already exists;
4. uses the B-tree replacement path when both rowids are equal;
5. otherwise deletes the old row and insert-only writes the new key and
   record, relying on the statement rollback mode selected by physical
   planning to restore the old row after any later failure; and
6. increments the VM-local change count exactly once only after the complete
   replacement succeeds.

An already-absent old row is a successful no-op. UPDATE replacement never
publishes a last-insert-rowid event.

The lowering module is generalized from `read_lowering` to canonical
`plan_lowering` naming when the first mutation program is added. The project
retains no compatibility aliases. Its public boundary uses:

```cpp
enum class PlanLoweringErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedPlan,
  kResourceLimit,
  kInternalInvariant,
};

struct PlanLoweringError;
using LowerPlanResult = std::expected<BytecodeProgram, PlanLoweringError>;

LowerPlanResult LowerPlan(const PhysicalPlan& plan,
                          ProgramLimits limits = {});
LowerPlanResult LowerPlan(const PhysicalMutationPlan& plan,
                          ProgramLimits limits = {});
```

The mutation overload initially accepts `PhysicalInsertMutation`.
Later mutation payloads return `kUnsupportedPlan` until their ordered slice
implements them. Invalid or moved-from plans return `kInvalidInput`.
Resource-limit failures retain the exact nested `ProgramError`; unexpected
bound, physical, or bytecode invariants return `kInternalInvariant`.

INSERT lowering reuses the complete existing scalar-expression emitter and
preserves this order:

1. allocate expression homes and scalar-call blocks;
2. allocate one contiguous register block in stored-column order, one
   separate resolved-rowid register, and one encoded-record register;
3. initialize ordinary stored columns from their materialized constant
   defaults or NULL, initialize the rowid alias field to NULL, and initialize
   the rowid register to NULL;
4. evaluate every bound VALUES expression from left to right, including
   duplicate expressions marked ineffective by binding;
5. write only effective ordinary targets into their stored-column registers
   and only the effective hidden-rowid or INTEGER PRIMARY KEY target into the
   rowid register;
6. open the write cursor, resolve or generate the rowid, build the table
   record, insert it, close the cursor, and halt.

The write descriptor owns the target root, stored-column affinity and NOT
NULL metadata, materialized ordinary-column default constants, and rowid
alias position. A rowid-alias default is ignored, matching SQLite rowid
generation. INSERT programs request write access, map physical atomicity to
`ProgramRollbackMode`, publish change count and user last-insert-rowid
metadata, and publish no result columns because RETURNING is deferred.

DELETE lowering uses the same scalar-expression emitter and converts all
three `PhysicalDeleteMutation` access kinds:

- `kEmpty` halts without opening a cursor;
- `kRowIdLookup` evaluates guards once, opens read and write descriptors for
  the target, evaluates the lookup key once, snapshots the positioned row,
  closes the read cursor, evaluates residual predicates, and point-deletes a
  match; and
- `kTableScan` evaluates guards once, opens read and write descriptors,
  rewinds, and applies the reopen-and-strictly-greater loop from Section 6.

DELETE register layout adds one contiguous source snapshot block in bound
source-column order and one current-rowid register after expression homes and
scalar-call argument blocks. While the read cursor is positioned, lowering
copies every bound source field plus the rowid into those registers and then
closes the cursor. Subsequent `BoundColumnExpression` and
`BoundRowIdExpression` lowering copies from the snapshot registers rather
than touching storage. No residual predicate, row-dependent scalar call, or
write instruction executes while the positioned read cursor or any page pin
remains open.

Scan rejection and successful deletion converge on one advance block. It
reopens the read cursor and emits `SeekRowIdInstruction(kGreater)` against
the owned current rowid. A missing seek closes both cursors and halts. Exact
lookup missing/residual-rejection paths likewise close every cursor without
publishing a change.

DELETE programs request write access, map physical atomicity to
`ProgramRollbackMode`, publish change-count metadata, never publish a
last-insert-rowid event, and have no result columns.

The first UPDATE lowering slice handles:

- `kEmpty`;
- exact-rowid lookup with either a stable or changed rowid; and
- table scans whose assignments are proven not to change rowid.

It reuses the DELETE source snapshot and cursor-safe exact/scan control flow.
After a candidate passes residual predicates, UPDATE:

1. copies every old stored column from the source snapshot into one
   contiguous replacement-value block;
2. copies the old rowid into a separate new-rowid register;
3. evaluates every assignment expression from left to right, including
   duplicate assignments marked ineffective by binding;
4. writes only effective ordinary assignments into their replacement-column
   registers and only the effective hidden-rowid or INTEGER PRIMARY KEY
   assignment into the new-rowid register;
5. requires the new rowid to convert losslessly to int64;
6. builds the complete replacement record; and
7. emits `UpdateTableInstruction` with the old rowid, new rowid, and record.

The source snapshot remains immutable while assignment expressions execute,
so every right-hand side observes the original row even when an earlier
assignment targets the same column. Exact rowid-changing UPDATE uses
statement rollback as selected by the physical plan. Stable-rowid scans use
the same reopen-and-strictly-greater loop as DELETE.

Scan UPDATE with `collect_original_rowids=true` remains `kUnsupportedPlan` in
this slice. Its following slice adds the bounded stable original-rowid
collection required to prevent a moved row from being revisited.

UPDATE programs publish change-count metadata, never publish a
last-insert-rowid event, and have no result columns.

Stable rowid collection uses one VM-owned ordered `std::vector<int64_t>` and
four typed instructions:

- `ClearRowIdListInstruction`;
- `AppendRowIdListInstruction`;
- `RewindRowIdListInstruction`; and
- `NextRowIdListInstruction`.

The list is statement-local VM state, not a general temporary table, sorter,
or public container. `Clear` establishes an empty list. `Append` losslessly
converts one initialized register and preserves append order. `Rewind`
publishes the first rowid or branches when empty. `Next` publishes the next
rowid and branches back to the caller's loop while entries remain.

The verifier tracks the list as one synthetic cursor-like lifecycle:
closed before `Clear`, unpositioned while collecting, and positioned while
iterating. It rejects append/rewind before initialization, append while
iterating, next before positioning, incompatible control-flow joins, invalid
registers, and invalid branch targets.

The VM bounds `list.size() * sizeof(int64_t)` by
`VmLimits::maximum_value_bytes`, returns `kTooLarge` before exceeding that
limit, propagates allocation failure, preserves deterministic append order,
and clears iteration state during halt, error cleanup, and reset. Reset also
empties the list before repeated execution.

VM construction remains preparation-scoped and storage-independent. The VM
owns its immutable program, bindings, registers, cursor slots, and resolved
function/collation registries across reset and repeated execution.

`VmEnvironment` contains only the synchronous function and collation
registries used during `Vm::Create()`. `VmExecutionContext` is attached later
and carries the Pager, catalog generation, and optional statement-scoped
`TransactionWriter`. A write program cannot attach a context without a
writer. A program that requires a database snapshot cannot attach without
the corresponding active Pager transaction.

Ordinary detach is rejected while a result row is suspended. Reset is the
explicit abort path: it closes runtime cursors, clears the row, and detaches
the context before returning ready. These rules make the statement layer's
capability lifetime explicit rather than relying on a prepared VM to retain
transaction-owned pointers.

After `BeginStatement()`, the session attaches one execution-scoped context
containing:

- the transaction's Pager for read cursors;
- the `TransactionWriter` capability for mutation;
- the catalog-generation scalar required by the program;
- function and collation registries; and
- a narrow VFS randomness capability for rowid selection.

All table roots, field layouts, affinities, defaults, constraints, and schema
requirements needed at runtime are lowered into immutable bytecode. The VM
never receives or depends on `CatalogSnapshot`.

The execution context is detached before `TransactionStatement::Succeed()`,
`Rollback()`, reset, error cleanup, or finalization. A VM without an attached
context cannot execute storage instructions. Prepared VM lifetime therefore
never extends the statement-scoped transaction capability.

The public transaction layer still exposes no unrestricted Pager or owning
B-tree session.

### 6. Keep scan mutation safe with point-operation B-tree APIs

Node 36 intentionally exposes point mutation, not a public writable traversal
cursor.

UPDATE and DELETE therefore use this first-version loop:

1. open a read cursor and seek the next rowid;
2. copy the rowid and every bound source field needed by expression
   evaluation into owned VM registers;
3. close the read cursor and release every page pin;
4. evaluate the predicate and replacement expressions;
5. perform one typed point delete/replace through `TransactionWriter`;
6. reopen a read cursor and seek strictly greater than the prior rowid; and
7. continue until exhausted.

This preserves ownership and prevents a read pin from aliasing in-place
mutation. It is `O(n log n)` for a full scan and becomes a baseline for Node
40. It does not invent a second mutation algorithm or retain raw cell views
across page edits.

The reopen-and-seek loop is used only for DELETE and UPDATE plans proven not
to assign the rowid/IPK alias.

For a scan UPDATE that may change rowid, the VM first collects the complete
ordered set of qualifying **original** rowids in owned memory, closes the
scan, and then point-reads and mutates only that stable set. A row moved to a
greater rowid is therefore never revisited. Allocation failure while
collecting rowids occurs before the first mutation. Direct exact-rowid UPDATE
may remain one-pass because it has only one target.

The bounded rowid collection is statement-owned and subject to the existing
maximum allocation/value limits. It is not a general temporary-table or sort
mechanism.

### 7. Implement rowid-table record and constraint semantics

INSERT maps input columns to the table's stored record shape.

- The INTEGER PRIMARY KEY alias is stored as NULL in the record and supplied
  as the B-tree integer key.
- An explicit non-NULL rowid alias must convert losslessly to int64.
- NULL or omitted rowid selects a new rowid.
- Missing ordinary columns use a supported constant default or NULL.
- Affinity is applied before record encoding.
- NOT NULL is checked after affinity and before B-tree mutation.
- Duplicate rowid returns `kConstraint`.

New rowid selection mirrors SQLite:

1. empty table selects 1;
2. otherwise use the largest integer key plus one whenever that key is below
   `INT64_MAX`, including transitions from negative keys through rowid 0;
3. at `INT64_MAX`, apply SQLite's positive-candidate transformation to VFS
   random values and probe at most 100 candidates; and
4. return `kFull` when all 100 candidates collide.

AUTOINCREMENT and `sqlite_sequence` are deferred.

The same helper allocates rowids for user rowid tables and for
`sqlite_schema`; it does not use a separately invented schema-row sequence.

UPDATE computes a complete replacement row. A rowid change performs
delete-then-insert inside the statement savepoint. Same-rowid replacement uses
the existing B-tree replacement path.

DELETE reports one change for every removed row.

All multi-row UPDATE/DELETE statements request anonymous statement rollback.
Any expression, allocation, constraint, Pager, or B-tree error restores the
entire statement while retaining an explicit outer transaction when safe.

### 8. Perform CREATE TABLE as one catalog mutation statement

CREATE TABLE execution:

1. validates the complete supported definition, duplicate-name behavior, and
   reserved-name rules;
2. reserves the next catalog generation before mutation;
3. starts CREATE with `StatementRollbackMode::kStatement`;
4. initializes an empty database when required;
5. creates the table B-tree root;
6. chooses the next `sqlite_schema` rowid;
7. inserts the schema record with type `table`, name, table name, root page,
   and canonical stored CREATE SQL;
8. increments the schema cookie on page 1;
9. loads an **unpublished candidate catalog** from the still-active
   transaction using the reserved generation;
10. rolls the CREATE statement back if candidate loading fails;
11. calls `TransactionStatement::Succeed()` only after candidate validation;
    and
12. publishes the candidate snapshot only when the coordinator outcome
    retains the DDL.

For autocommit CREATE, a durable commit with only final cleanup remaining
retains the candidate and publishes it once cleanup succeeds. It is never
discarded after the schema is already durable.

`IF NOT EXISTS` returns success without mutation when a table of the same
case-insensitive name already exists. Any conflicting object kind without
`IF NOT EXISTS` returns SQLite-compatible generic/schema error behavior.

Object names beginning with `sqlite_`, compared case-insensitively, are
reserved and rejected before mutation.

Stored schema SQL is built exactly as:

```text
"CREATE TABLE " + retained source from the unqualified table-name token
through the final table option
```

It excludes schema qualification, `IF NOT EXISTS`, preceding syntax, and the
terminating semicolon while preserving quoting, spacing, column definitions,
and supported table options in the retained slice.

The B-tree layer gains only typed page-1 helpers required for:

- schema cookie read/increment;
- schema-table rowid selection; and
- schema row insertion through the existing table writer.

Persistent format code still performs no filesystem I/O.

Prepared statements compiled against the old catalog generation reprepare
before their next execution. Statements active during schema mutation are
rejected as busy by the writable session's sequential statement scheduler.

Full transaction rollback or `ROLLBACK TO` may undo an earlier successful
CREATE. Those operations mark the in-memory catalog stale. Before any later
prepare or execution, the session reserves a new generation and reloads the
catalog from the restored Pager snapshot. A reload failure blocks statement
execution and does not expose roots from the rolled-back schema.

### 9. Map SQL transaction statements directly to the coordinator

Transaction-control programs contain no storage bytecode loop. The writable
session's transaction-SQL adapter maps their immutable bound form to:

- `Begin(kDeferred|kImmediate)`;
- `Commit()`;
- full `Rollback()`;
- named `Savepoint()`;
- `Release()`; and
- `RollbackTo()`.

They still use normal prepare, bind-count, step, reset, finalize, error, and
automatic re-execution lifecycle. One successful step returns `kDone`.

The adapter translates coordinator API-precondition errors for SQL semantics:

- nested BEGIN;
- COMMIT without an active transaction; and
- ROLLBACK without an active transaction

map from coordinator `kMisuse` to `kGeneric`/`SQLITE_ERROR` with
SQLite-compatible diagnostics. It preserves genuine C++ API misuse for
moved-from/invalid objects and propagates `kBusy`, constraint, OOM, I/O, and
Pager failures unchanged.

Transaction SQL is invalid through `ReadSession`.

### 10. Preserve statement and connection lifecycle semantics

The writable session permits one active statement, matching the Node 37
coordinator boundary. Preparing while another statement is active is allowed;
stepping a second statement returns `kBusy`.

On first step:

- SELECT requests `StatementAccess::kRead`;
- INSERT/UPDATE/DELETE/CREATE TABLE request `kWrite`;
- mutation plans select statement-savepoint or transaction rollback mode; and
- transaction-control statements call the coordinator directly.

On VM completion the session:

1. captures the VM-local row-change count;
2. closes every VM cursor and detaches the execution context;
3. calls `TransactionStatement::Succeed()`; and
4. publishes `changes()` only after that succeeds.

The VM publishes `last_insert_rowid()` at the successful user-table INSERT
instruction, before statement completion, and that value is intentionally not
rolled back.

On VM error the session detaches the context, calls `Rollback()`, publishes
zero changes, and preserves the primary statement error augmented with
rollback failure context.

Reset and finalize return the prior execution error, close cursors, and finish
any still-active transaction statement explicitly. Destructors perform no
hidden commit. The statement destructor may abandon the coordinator token;
the session then permits only full rollback, as defined by ADR-0047.

### 11. Keep first-version schema and index boundaries explicit

Node 38 does not execute CREATE INDEX, DROP, ALTER, VACUUM, PRAGMA writes,
attached-database writes, or temp-schema writes.

It does not mutate tables that have:

- explicit indexes;
- UNIQUE or non-rowid PRIMARY KEY automatic indexes;
- WITHOUT ROWID storage;
- triggers;
- foreign keys requiring actions; or
- virtual-table ownership.

Those cases fail before mutation. Node 41 adds index-aware planning and index
maintenance. Node 42 adds broader SQL and schema semantics.

## Test Strategy

Implementation proceeds in reviewed red-green slices.

### Syntax and immutable contracts

- INSERT, UPDATE, DELETE, and transaction grammar normal/boundary vectors;
- exact spans, names, assignment order, column lists, and unsupported tails;
- AST validation of duplicate assignments and invalid references;
- parser allocation failure and depth/list limits; and
- existing SELECT/CREATE parser behavior unchanged.

### Binding and plans

- table/column/rowid alias resolution;
- duplicate/missing target columns;
- affinity, defaults, nullability, and parameter slots;
- rejection of indexed/unsupported tables before mutation;
- direct rowid lookup versus scan plans;
- schema version requirements; and
- no AST/planner type in VM or storage.

### VM and statement behavior

- single-row insert with explicit and generated rowids;
- negative-to-zero rowid generation, max rowid, and exact 100-probe random
  fallback;
- duplicate rowid and NOT NULL errors;
- update no rows, one row, all rows, rowid change, and expression evaluation;
- delete no rows, one row, and all rows;
- statement rollback after the first, middle, and final row mutation;
- changes and last-insert-rowid;
- implicit and explicit transactions and named savepoint SQL;
- reset/finalize/error retry and schema reprepare; and
- escaped handles/cursors invalidated safely;
- final writable-session owner best-effort rollback without hidden commit;
- changes publication only after statement success; and
- last-insert-rowid retention across statement and transaction rollback.

### CREATE TABLE and catalog

- empty database initialization;
- exact `sqlite_schema` row fields and SQL text;
- qualified input names, IF NOT EXISTS, quoting/spacing preservation, and
  canonical stored SQL;
- case-insensitive `sqlite_` reserved-name rejection;
- root creation and schema rowid allocation;
- schema-cookie increment;
- IF NOT EXISTS;
- duplicate names and case folding;
- catalog reload and prepared statement expiration;
- candidate-catalog load failure rolling back CREATE;
- catalog reload after full rollback and rollback-to undo CREATE;
- every supported/rejected constraint boundary; and
- SQLite opens the image and passes `PRAGMA integrity_check`.

### Differential, crash, and failure evidence

Pinned SQLite traces compare:

- rows and value types;
- primary/extended errors;
- changes and last insert rowid;
- transaction/autocommit state;
- SQL-level transaction error-code translation;
- schema rows/cookies;
- freelist counts; and
- final database bytes where deterministic.

Deterministic cuts cover each DML/DDL statement before mutation, after each
row mutation, during catalog mutation, during statement rollback, and during
implicit/explicit commit. Recovery must yield the complete old statement or
complete successful statement within the correct outer transaction image.

OOM matrices cover parser/AST growth, binding/planning/lowering, VM setup,
record encoding, row collection, catalog SQL ownership, and every
coordinator/storage growth boundary.

### Model and performance evidence

A reference model executes mixed CREATE, INSERT, UPDATE, DELETE, SELECT,
statement error, explicit transaction, and savepoint traces against standard
containers and pinned SQLite.

A fixed-work baseline records:

- rows examined and changed;
- B-tree seeks, cursor reopens, page visits, writes, syncs, and allocations;
- implicit versus explicit transaction cost; and
- CREATE TABLE catalog work.

Matched native write performance remains Node 40.

## Consequences

### Positive

- The writable MVP reaches SQL through the same immutable compile pipeline as
  SELECT.
- Transaction and statement atomicity are inherited from the reviewed Node 37
  coordinator.
- Catalog mutation, schema cookie publication, and B-tree root creation occur
  in one rollback-safe statement.
- Unsupported index-bearing tables are rejected rather than silently
  corrupted.
- The first UPDATE/DELETE algorithm is simple, pin-safe, and measurable.

### Negative

- UPDATE/DELETE full scans initially reopen the B-tree cursor after each
  mutation.
- The writable session remains single-active-statement.
- Supporting a small constraint subset means the parser can represent schema
  forms that writable execution still rejects.
- Renaming the VM types touches existing read code and tests.

### Deferred

- CREATE INDEX and index maintenance;
- multi-row VALUES and INSERT SELECT;
- UPSERT, REPLACE, conflict clauses other than default ABORT, and RETURNING;
- advanced constraints, CHECK, UNIQUE, foreign keys, triggers, generated
  columns, views, and virtual tables;
- WITHOUT ROWID, STRICT, AUTOINCREMENT, and `sqlite_sequence`;
- DROP/ALTER and other schema statements;
- overlapping writable statements;
- attached databases, super-journals, concurrency, busy handlers, and WAL;
- cost-based write planning; and
- matched write-performance thresholds.

## References

- `src/parse.y`
- `src/insert.c`
- `src/update.c`
- `src/delete.c`
- `src/build.c`
- `src/vdbe.c`
- ADR-0027: Immutable Source-Preserving Syntax Trees
- ADR-0028: Pure SQLite-Compatible SQL Parser
- ADR-0029: Immutable Catalog Snapshots
- ADR-0031: Typed Immutable Bytecode Programs
- ADR-0032: Read-Only Bytecode Virtual Machine
- ADR-0037: RAII Read Session API
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
