# ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery

- Status: Accepted
- Date: 2026-10-08

## Context

The completed index-planning node provides:

- a source-preserving parser for the current single-source statement surface;
- immutable bound expressions, logical plans, physical plans, and verified
  bytecode;
- table, rowid, and ordinary-index access with covering and noncovering
  execution;
- scalar functions, collations, affinity, record encoding, and complete
  SQLite B-tree comparison;
- statement-atomic indexed DML, CREATE TABLE, CREATE INDEX, transactions, and
  savepoints;
- immutable catalog snapshots with tables, indexes, CHECK syntax, defaults,
  and STAT1 estimates; and
- differential, crash, model, fuzz, sanitizer, allocation, and matched
  performance harnesses.

The active `implement-advanced-sql` node must add joins, sorting, aggregates,
subqueries, CTEs, views, constraints, triggers, and the remaining declared
core SQL surface. These features cannot be implemented as unrelated parser
patches:

- ORDER BY, DISTINCT, GROUP BY, compounds, recursive CTEs, aggregate-local
  ordering, and window functions all require statement-owned temporary
  records with SQLite collation and NULL semantics.
- Joins require multiple source scopes, ambiguous-name handling, parameterized
  inner access paths, and outer-join null extension.
- Scalar, EXISTS, IN, FROM-clause, and correlated subqueries require nested
  query blocks plus reusable or repeated execution.
- Views and CTEs expose query results as sources and require cycle detection,
  derived column metadata, and controlled materialization.
- CHECK, UNIQUE, foreign-key, generated-column, and trigger behavior must be
  integrated into the existing statement-atomic mutation ordering rather than
  executed as post-write callbacks.
- Recursive CTEs, triggers, and reusable subqueries require verified
  subroutines or coroutines. Ad hoc recursive calls from the VM to the
  compiler would violate the dependency DAG.

Pinned SQLite 3.54.0 defines the relevant mechanisms in:

- `src/parse.y:509-959`, where views, WITH clauses, compounds, SELECT cores,
  VALUES, source lists, joins, ORDER BY, GROUP BY, and HAVING are represented
  as one recursive SELECT grammar;
- `src/parse.y:1738-1967`, where trigger programs and CTE definitions are
  parsed;
- `src/resolve.c:1765-2295`, where SELECT blocks, outer name contexts,
  aggregate use, aliases, result ordinals, and correlated references are
  resolved;
- `src/select.c:3013-3800`, where compound SELECT operators stream or merge
  rows through explicit destinations;
- `src/select.c:7709-9045`, where query preparation, source subqueries,
  coroutines, WHERE loops, DISTINCT, grouping, aggregation, sorting, and
  result destinations are coordinated;
- `src/where.c:6769-7774`, where nested-loop join order and loop code are
  selected;
- `src/vdbesort.c:1-150`, where a sorter retains records in memory, spills
  sorted packed-memory arrays to temporary files at a bounded threshold, and
  performs incremental merge;
- `src/vdbe.c:1170-1288`, where subroutine and coroutine control flow is
  executed;
- `src/vdbe.c:1340-1390`, where each reached halt carries its own conflict
  action;
- `src/vdbe.c:4652-4826` and `src/vdbe.c:6172-6778`, where ephemeral B-trees,
  sorter cursors, sorter comparison, iteration, and insertion are executed;
- `src/vdbe.c:7982-8160`, where aggregate step and final operations own
  function state;
- `src/vdbeaux.c:3401-3461`, where `OE_Fail` retains prior statement changes
  by releasing the statement savepoint while `OE_Abort` rolls it back and
  stronger actions roll back the transaction;
- `src/expr.c:3914-4050`, where scalar and EXISTS subqueries become reusable
  subroutines with LIMIT 1 and correlated-execution rules;
- `src/build.c:3069-3150`, where CREATE VIEW stores a copied unresolved
  SELECT and forbids parameters, and `src/build.c:3152-3290`, where result
  columns and circular definitions are checked lazily when the view is used;
- `src/insert.c:1889-2820`, where NOT NULL, CHECK, UNIQUE, conflict actions,
  generated values, and replacement ordering are checked before final writes;
- `src/fkey.c:889-1450`, where foreign-key checks and actions are integrated
  around row mutation; and
- `src/pragma.c:1165-1180`, where foreign-key enablement and deferred
  constraint controls are connection state with transaction restrictions;
- `src/trigger.c:104-324` and `src/trigger.c:1402-1544`, where trigger schema
  objects, OLD/NEW usage, column masks, and row-trigger programs are built and
  invoked.

SQLite combines these responsibilities across mutable `Select`, `Expr`,
`SrcList`, `AggInfo`, `Vdbe`, and `Parse` structures. Modern SQLite must
preserve observable semantics and the proven execution mechanisms without
copying those source-level cycles.

## Decision

### 1. Deliver the node as ordered vertical slices

`implement-advanced-sql` is an oversized node under ADR-0046. It is delivered
through sequential reviewed pull requests. Each production slice:

1. adds only the syntax needed by that slice;
2. extends immutable binder, logical, and physical contracts;
3. adds typed bytecode and VM capabilities only when required;
4. reaches the session API end to end;
5. begins with a focused failing behavioral test;
6. includes differential, malformed, OOM/allocation, model/fuzz, and
   performance evidence appropriate to the behavior; and
7. is merged before the next slice begins.

This ADR fixes the architecture, ordering, and completion surface. A slice
that introduces a new persistent transition, temporary-storage format,
function-state contract, or trigger/foreign-key ordering requires a focused
child ADR before implementation.

### 2. Extend syntax through immutable arenas, not pointer graphs

The syntax layer remains pure and source preserving.

Advanced syntax adds strong IDs and owned arena vectors for:

- query blocks and compound terms;
- source terms and join operators;
- ORDER BY and GROUP BY terms;
- CTE definitions;
- subquery expressions and FROM-clause subqueries;
- view definitions;
- trigger definitions and trigger steps; and
- window definitions when that slice begins.

Expressions refer to nested queries by strong ID. Query blocks refer to
expressions and sources by strong ID. No syntax node owns a raw pointer,
catalog object, plan node, register, cursor, or runtime state. Destruction
remains nonrecursive.

The parser accepts no prefix of an unmodeled production. Existing
`kUnsupportedSyntax`, depth, column, source-size, and tail-offset contracts
remain authoritative. Every slice updates the grammar-shaped malformed-input
matrix before it broadens acceptance.

### 3. Bind one explicit query-block scope tree

The binder owns immutable query blocks. Each block contains:

- ordered source descriptors;
- the visible source-column namespace;
- result expressions and metadata;
- WHERE, grouping, HAVING, ordering, window, limit, and compound metadata as
  applicable;
- a link to the permitted outer query scope; and
- correlation metadata derived during binding.

Bound column references identify both source and column. Unqualified lookup
searches the current block first and reports `kAmbiguousColumn` when multiple
visible sources match. Correlated lookup proceeds through outer blocks only
after the current block has no match. Qualified lookup never falls back to a
result alias or an unrelated outer source.

Each joined source list also publishes an immutable joined-output namespace.
It contains:

- ordinary visible columns with source provenance;
- merged NATURAL/USING columns with left and right provenance;
- a synthesized value expression when an outer join requires coalescing the
  two sides;
- result metadata and collation; and
- wildcard publication order.

Unqualified lookup and unqualified `*` use this joined-output namespace, so a
USING column appears once. Qualified `left.column`, `right.column`,
`left.*`, and `right.*` continue to address the separate base sources. A
merged column is therefore not forced into one base source identity.

SQLite clause-specific alias and ordinal rules are explicit:

- SELECT result aliases are visible only in clauses where pinned SQLite
  permits them;
- integer ORDER BY and GROUP BY terms resolve as result ordinals at the same
  phase as SQLite;
- source columns win over same-named result aliases where SQLite gives them
  precedence;
- aggregate and window legality is checked per query block; and
- views, CTEs, and derived tables publish immutable result-column metadata.

Binding never chooses join order, materialization, sorter layout, registers,
or bytecode control flow.

### 4. Add statement-owned temporary relational storage below the VM

Sorting and materialization use shared lower-layer capabilities rather than
feature-local containers.

The temporary-storage module depends only on accepted base, coding, record,
collation, VFS, pager, and B-tree contracts. It does not depend on syntax,
catalog, binder, planner, lowering, VM, session, or API types.

The session owns an immutable temporary-storage configuration and a narrow
factory backed by its VFS. `VmExecutionContext` borrows the factory beside
the existing execution-scoped pager and transaction capabilities. The VM
requests statement-owned sorter or ephemeral-relation handles; it never
opens a platform file directly and never retains the session.

It provides two typed capabilities:

- an ordered record sorter with write, rewind, current-record, next, compare,
  reset, and close states; and
- an ephemeral keyed relation for membership, uniqueness, queue, and
  materialized-row access.

Both consume complete encoded records plus immutable comparison descriptors.
They reuse `SqlValue`, record encoding, collations, sort direction, and NULL
ordering rather than defining a second value or comparator system.

The sorter follows SQLite's single-threaded external merge mechanism first:

1. retain owned records in bounded memory;
2. sort an in-memory run with the canonical record comparator;
3. spill a packed run to a pathless delete-on-close temporary VFS file when
   the configured threshold is exceeded;
4. finish pending runs at rewind;
5. merge a bounded number of runs incrementally; and
6. release every temporary file and allocation on reset, halt, error, or
   destruction.

The pinned benchmark profile is single-threaded, so worker sort threads are
not introduced. The transient spill encoding is internal and not a database
compatibility format, but its bounds, state transitions, ordering, fault
cleanup, and merge behavior are reference faithful.

Ephemeral keyed relations reuse the existing temporary VFS, pager, record,
and B-tree mechanisms where their invariants fit. A child ADR must justify any
different representation before it is added. No unbounded in-memory
`std::set`, hash table, or vector is accepted as the only implementation for
SQL whose cardinality is controlled by database contents.

### 5. Extend verified bytecode with capability lifecycles

Advanced execution remains typed bytecode. The VM never receives AST, binder,
logical-plan, physical-plan, or catalog objects.

Bytecode families are added only by the slice that needs them:

- sorter open, insert, rewind, current-record, compare, next, reset, and
  close;
- ephemeral relation open, insert, seek, delete, rewind, next, duplicate, and
  close;
- aggregate initialize, step, inverse when later required, final, and reset;
- subroutine begin, call, return, and once;
- coroutine initialize, yield, end, and resume;
- null-row publication for outer joins; and
- trigger program entry, OLD/NEW row access, RAISE handling, and return.

The verifier tracks each capability's exact lifecycle and rejects:

- use before open or initialization;
- writes after rewind;
- reads before positioning;
- incompatible states at control-flow joins;
- invalid subroutine or coroutine targets;
- aggregate finalization before initialization or after finalization;
- trigger access outside the declared OLD/NEW event shape; and
- reachable fallthrough beyond a program or subprogram.

VM cleanup owns all sorter, ephemeral, aggregate, coroutine, and trigger
state on halt, error, reset, finalize, and destruction.

Every constraint, trigger RAISE, and other conflict-producing failure edge
carries its effective completion disposition:

- `ABORT` returns the error and rolls back the anonymous statement savepoint;
- `FAIL` returns the error while retaining earlier changes from the statement
  or trigger program, and an autocommit statement durably commits that
  retained prefix;
- `ROLLBACK` returns the error and rolls back the transaction;
- `IGNORE` skips only the conflicting row or trigger step as SQLite defines;
  and
- `REPLACE` performs the required pre-delete actions before insertion.

One statement or trigger program may contain different actions at different
failure sites. The VM therefore returns the action from the edge actually
reached as part of its typed failure outcome. The session and transaction
coordinator apply that runtime action and current transaction state.
Foreign-key violations select ABORT as pinned SQLite does. A generic "every
VM error rolls back the statement" fallback is forbidden once non-ABORT
conflict actions are accepted.

### 6. Preserve SQLite evaluation phases in logical and physical plans

Logical plans become query-block DAGs rather than one linear single-source
tree. They represent semantic phases without runtime resources:

1. source production;
2. join and ON/USING filtering;
3. WHERE filtering;
4. grouping and aggregate evaluation;
5. HAVING filtering;
6. window evaluation;
7. projection;
8. DISTINCT or compound set behavior;
9. ORDER BY; and
10. OFFSET/LIMIT.

Physical plans choose mechanisms:

- nested-loop joins with parameterized table, rowid, or index inner access;
- measured automatic transient indexes for unindexed equality joins when the
  nested-loop baseline exceeds the accepted guard;
- null-extension boundaries for outer joins;
- index-order delivery or a sorter;
- streaming or materialized subqueries;
- sorted grouping;
- ephemeral membership for DISTINCT and set operators;
- queue relations for recursive CTEs; and
- trigger and constraint subprograms around mutation.

The first correct join implementation is nested loop, matching SQLite's
WHERE-loop model. Inner joins may be reordered only when dependency,
correlation, CROSS JOIN, and outer-join barriers permit it. Outer joins
preserve the declared null-extension boundary. Hash and merge joins are not
introduced without a later measured requirement.

The initial join slice records a scaling baseline before optimization. A
later slice in this same node builds SQLite-aligned automatic covering
indexes in the shared ephemeral relation when fixed-work evidence shows the
repeated inner scan gap. Bloom filtering is added only with that measured
automatic-index path; a hash join is not substituted for it.

### 7. Use one aggregate-function contract

The function registry gains aggregate descriptors alongside scalar
descriptors. An aggregate descriptor defines:

- name and arity;
- deterministic and collation requirements;
- a typed state factory;
- step;
- final;
- optional inverse/value operations only for the later window slice; and
- explicit state destruction.

State ownership is RAII and statement local. Expected aggregate failures
return typed errors. No function receives a VM, parser, catalog, or raw
owning pointer.

Aggregate execution also owns one representative-row snapshot per group.
Every aggregate step reports whether that snapshot must change. Ordinary
bare columns use SQLite's representative-row behavior. A query with exactly
one built-in MIN or MAX couples the snapshot to the row that supplied the
current extremum; multiple MIN/MAX calls and ties follow pinned SQLite's
observed update order. Snapshot allocation, replacement, reset, and failure
cleanup are part of the aggregate child ADR.

The first aggregate slice covers SQLite-compatible COUNT, SUM, TOTAL, AVG,
MIN, MAX, and GROUP_CONCAT semantics, including DISTINCT arguments, FILTER
when its expression grammar is accepted, NULL handling, integer overflow,
collation, empty input, and SQLite's permitted bare result columns. Ordered
aggregate arguments are added only after the shared sorter contract is live.

Grouping uses ordered records first so one mechanism serves GROUP BY,
aggregate DISTINCT, and deterministic group boundaries. A hash-only
aggregation path is deferred until matched measurements justify it.

### 8. Implement subqueries through explicit destinations and reuse policy

Every nested query compiles as a plan fragment with a declared destination:

- scalar register range;
- EXISTS flag;
- IN membership relation;
- coroutine row stream;
- materialized ephemeral relation;
- recursive queue; or
- discarded rows.

Scalar and EXISTS subqueries apply SQLite's implicit LIMIT 1 behavior.
Uncorrelated, variable-free subqueries may execute once and reuse their
result. Correlated subqueries execute at the required outer-row frequency.
IN/NOT IN preserves SQLite's NULL and empty-set semantics.

FROM-clause subqueries and views may be flattened only after a reviewed rule
set proves semantic equivalence. The first correct implementation may choose
coroutine or materialization conservatively; it may not silently duplicate or
elide non-deterministic expressions.

### 9. Integrate views, constraints, and triggers with catalog snapshots

Catalog snapshots add typed view and trigger descriptors rather than treating
their `sqlite_schema` rows as tables.

Views:

- retain their parsed immutable SELECT definition;
- remain unresolved in the catalog snapshot;
- forbid parameters;
- retain optional declared column names;
- derive and validate result-column names, count, and metadata lazily when
  the binder expands the view;
- expose no rowid;
- detect direct and indirect definition cycles with a binder-owned expansion
  stack; and
- bind through the same nested query-block pipeline as a FROM subquery.

CREATE VIEW therefore may succeed when a dependency is absent or the
definition is circular. The compatible error occurs when the view is first
expanded or its metadata is requested, not during catalog loading or CREATE.

Constraints:

- CHECK expressions and generated columns bind in a table-definition scope;
- UNIQUE and non-rowid PRIMARY KEY reuse automatic-index metadata and the
  reviewed index-maintenance path;
- conflict actions are selected before persistent mutation;
- foreign-key checks and actions are statement/transaction aware; and
- STRICT, WITHOUT ROWID, AUTOINCREMENT, and generated-column write semantics
  receive focused child ADRs before activation.

Constraint execution includes connection-owned controls for
`foreign_keys` and `defer_foreign_keys`. Their defaults match pinned SQLite.
Savepoints snapshot and restore deferred-violation counters, which are checked
at the compatible RELEASE/COMMIT boundaries. The `defer_foreign_keys`
connection setting itself is not savepoint state: it remains unchanged across
ROLLBACK TO and resets at the compatible top-level COMMIT/ROLLBACK boundary
or when explicitly disabled.

The initial values are `foreign_keys = OFF` and
`defer_foreign_keys = OFF`. Changing `foreign_keys` inside an active
transaction follows SQLite's no-op/restriction behavior defined by the child
ADR; it is not silently applied mid-transaction.

Triggers:

- are immutable catalog objects with timing, event, target, optional WHEN,
  update-of columns, and an ordered step program;
- bind OLD and NEW as explicit pseudo-sources;
- compile to verified subprograms;
- preserve SQLite BEFORE/AFTER/INSTEAD OF ordering and RAISE behavior;
- obey one explicit recursion-depth contract; and
- execute inside the owning statement transaction so the reached failure edge
  rolls back for ABORT/ROLLBACK or retains the compatible prefix for FAIL.

Trigger execution includes a connection-owned `recursive_triggers` control
with pinned default `OFF`. Its value and recursion-depth state are explicit
session inputs to compilation/execution rather than hidden globals.

No trigger callback performs parser, binder, or planner work at VM runtime.

### 10. Deliver slices in dependency order

The canonical slice order is:

1. this architecture ADR and pinned behavior audit;
2. ORDER BY with the shared spill-capable sorter, bounded ordering relation,
   and exact sort/LIMIT semantics;
3. DISTINCT, VALUES, and compound SELECT, introducing the shared keyed
   ephemeral relation with its first consumers;
4. aggregate registry, GROUP BY, HAVING, aggregate-local DISTINCT, and
   representative-row semantics;
5. multi-source binding plus comma, CROSS, INNER, and LEFT joins;
6. a correctness and scaling baseline followed by automatic transient
   covering indexes for unindexed equality joins;
7. RIGHT, FULL, NATURAL, and USING join completion through the joined-output
   namespace;
8. scalar, EXISTS, IN, and FROM-clause subqueries with correlation;
9. ordinary and recursive CTEs with materialization hints;
10. CREATE VIEW and lazy view expansion, followed by CREATE TABLE AS SELECT and
   INSERT SELECT;
11. expression and partial index creation/maintenance, executable
    non-constant defaults, and the remaining currently rejected ordinary
    index shapes;
12. advanced DML forms, ABORT/FAIL/IGNORE/REPLACE/ROLLBACK completion,
    RETURNING, and generated columns;
13. named constraints, column COLLATE behavior, CHECK, and complete UNIQUE
    and PRIMARY KEY behavior;
14. STRICT and AUTOINCREMENT with `sqlite_sequence`;
15. WITHOUT ROWID writes only after complete primary-key and secondary-index
    maintenance is available;
16. connection controls for `foreign_keys` and `defer_foreign_keys`, followed
    by immediate/deferred foreign-key checks and actions;
17. `recursive_triggers`, CREATE TRIGGER, and trigger execution;
18. window functions over the sorter, aggregate, and coroutine foundations;
19. remaining declared core schema operations such as DROP and reviewed
    ALTER TABLE forms; and
20. integrated compatibility, crash, model, fuzz, allocation, and matched
    performance evidence followed by whole-node review.

Each slice may be split further, but the order cannot be inverted when a
later mechanism depends on an earlier capability. In particular, no feature
adds a private sorter, private ephemeral table, private aggregate state, or
unverified recursive executor to bypass its prerequisite.

### 11. Close every current writable blocker explicitly

The node owns or explicitly assigns every mutation rejection inherited from
ADR-0048 and ADR-0052:

| Current blocker | Owning slice or node |
|---|---|
| expression indexes | advanced-index slice 11 |
| partial indexes | advanced-index slice 11 |
| non-constant executable defaults | advanced-index/default slice 11 |
| unsupported ordinary index term or suffix shapes | advanced-index slice 11 |
| non-rowid PRIMARY KEY and complete UNIQUE conflict behavior | constraint slice 13 |
| CHECK constraints | constraint slice 13 |
| column COLLATE constraints | constraint slice 13 |
| named column and table constraints | constraint slice 13 |
| generated columns | advanced-DML slice 12 |
| STRICT tables | table-semantics slice 14 |
| AUTOINCREMENT and `sqlite_sequence` | table-semantics slice 14 |
| WITHOUT ROWID table and secondary-index mutation | slice 15, after slice 13 |
| IGNORE, REPLACE, FAIL, and ROLLBACK conflict actions | advanced-DML slice 12 |
| foreign-key checks and actions | configuration/FK slice 16 |
| triggers | trigger slice 17 |
| views used as writable INSTEAD OF targets | view slice 10 plus trigger slice 17 |
| virtual-table mutation | `implement-extensions`, not this node |

A blocker is not silently dropped from the compatibility matrix. If research
shows that one row belongs to a later DAG node, the module graph and this
table are updated in an accepted ADR before the advanced-SQL node is marked
complete.

### 12. Require slice-specific compatibility and performance evidence

Every slice adds pinned SQLite differential coverage for:

- rows, row order where SQL defines it, storage classes, collations, NULLs,
  aliases, and duplicate names;
- primary error code and prepare-versus-step failure phase;
- malformed syntax and configured limits;
- statement reset, reprepare, and catalog-generation behavior;
- OOM at every allocation boundary and exact allocation budgets;
- deterministic model or fuzz actions where stateful behavior is added; and
- bidirectional database interoperability for persistent schema or mutation
  behavior.

Sorter and ephemeral-storage tests additionally cover:

- independent encoded-record golden vectors;
- in-memory, one-run, multi-run, and multi-level merge paths;
- equal keys, descending terms, NULL placement, collations, and sequence
  tie-breaking;
- memory-threshold boundaries;
- temporary open/read/write/sync/truncate/delete failures;
- reset and destruction at every lifecycle state; and
- no leaked file, page pin, or allocation after failure.

Persistent view, constraint, generated-column, foreign-key, schema, or trigger
transitions require exhaustive rollback-journal crash cuts. ABORT and
ROLLBACK outcomes recover to the exact compatible old image. Successful
outcomes recover to the exact committed image. FAIL outcomes recover to
either the old image or the exact retained-prefix image selected by the
executed failure edge and autocommit/transaction state. Pinned SQLite must
accept every recovered schema and content image.

Performance-sensitive slices record a correctness-complete baseline before
optimization. Matched configurations pin page size, cache size, journal mode,
synchronous mode, mmap, temp store, sorter memory threshold, schema, corpus,
SQL, result digest, and fixed work. Timing is uninstrumented; diagnostics and
profiles explain work but do not replace timing. Existing read, write, and
index baselines remain immutable.

## Explicit deferrals

This node does not implement:

- multi-connection locking or busy handling, which belongs to
  `implement-concurrency-locking`;
- WAL or shared-memory behavior, which belongs to `implement-wal`;
- C API, ABI, callback, and authorizer compatibility, which belongs to
  `implement-c-api-compatibility`;
- virtual tables and optional extensions, which belong to
  `implement-extensions`;
- attached databases, shared-cache mode, cross-database transactions, or TEMP
  schema persistence;
- parallel sorter workers, hash joins, merge joins, hash aggregation, or
  adaptive execution without a measured accepted requirement; or
- parser/planner/VM source structures copied from SQLite.

PRAGMA families other than the narrowly owned `foreign_keys`,
`defer_foreign_keys`, and `recursive_triggers` controls, plus ATTACH/DETACH,
VACUUM, and storage/concurrency configuration, are added only in the later
node that owns their underlying mechanism. This ADR does not authorize
success-shaped stubs for them.

## Consequences

### Positive

- Advanced features share one bounded temporary-record and control-flow
  foundation.
- The syntax, binder, planner, bytecode, VM, storage, and catalog dependency
  directions remain acyclic.
- Each SQL slice remains reviewable end to end.
- SQLite behavior is tested at the observable boundary while sorter,
  constraint, and persistent-transition mechanisms retain reference-faithful
  invariants.
- Existing indexes, transactions, crash recovery, and performance baselines
  remain reusable evidence rather than being bypassed.

### Negative

- The node requires many sequential pull requests.
- A correct spill-capable sorter and ephemeral relation precede several
  user-visible features.
- Conservative coroutine/materialization and nested-loop choices may be
  slower than later SQLite optimizations until matched evidence justifies
  them.
- Trigger, foreign-key, recursive CTE, and window verification substantially
  expand the bytecode state lattice and crash matrix.

## Rejected alternatives

### Implement each feature with a private container

Rejected because ORDER BY, DISTINCT, grouping, compounds, CTE queues,
subqueries, and aggregate-local ordering require the same record comparison,
boundedness, cleanup, and failure behavior.

### Keep every temporary row in memory

Rejected because SQL cardinality is database controlled. It would make valid
queries fail according to process memory rather than the configured SQLite
limits and temp-store policy.

### Reuse the persistent main-database transaction for temporary state

Rejected because statement-owned temporary state must not mutate the main
database, schema cookie, journal, or crash image.

### Execute nested plans directly from the VM

Rejected because it would couple the VM upward to planner and binder objects,
bypass bytecode verification, and make reset/error cleanup ambiguous.

### Compile triggers when each row fires

Rejected because runtime compilation violates layering, changes failure
phase, and cannot provide deterministic statement atomicity.

### Add hash joins or hash aggregation first

Rejected because pinned SQLite's proven first mechanism is nested loops plus
ordered/ephemeral records, and no matched benchmark currently demonstrates a
need for a different algorithm.

### Implement the whole grammar before executable slices

Rejected because a large syntax-only expansion would create unsupported AST
states, delay differential evidence, and make ownership/resource-limit review
unmanageable.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0006: AI-Native Sequential Workflow
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0028: Pure SQLite-Compatible SQL Parser
- ADR-0031: Typed Immutable Bytecode Programs
- ADR-0033: Immutable Read-Only SELECT Binding
- ADR-0034: Immutable Logical SELECT Plans
- ADR-0035: Deterministic Basic Read Optimization
- ADR-0036: Physical Read Plan Lowering
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0048: Writable SQL DML and CREATE TABLE Execution
- ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics
