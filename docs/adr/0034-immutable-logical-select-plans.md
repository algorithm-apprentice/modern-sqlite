# ADR-0034: Immutable Logical SELECT Plans

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0033 produces a complete immutable `BoundSelect`: names, source columns,
rowid references, functions, parameters, affinities, collations, result
metadata, predicates, and limits are already resolved. The next
dependency-graph node must expose the relational structure of that statement
without choosing a physical access path or emitting bytecode.

Pinned SQLite 3.54.0 does not have a stable logical-plan boundary:

- `src/select.c:7709-7728` begins one large `sqlite3Select()` routine that
  retains SELECT syntax, planning state, and VDBE code-generation state
  together;
- `src/select.c:8371-8473` prepares LIMIT state, invokes
  `sqlite3WhereBegin()` with the WHERE expression, emits the result loop, and
  closes the WHERE planner in one control flow;
- `src/select.c:1224-1226` applies OFFSET before loading result expressions;
- `src/select.c:1258-1344` loads result expressions inside the selected-row
  loop and routes them to the output destination;
- `src/select.c:2573-2618` evaluates LIMIT and OFFSET once into statement
  registers, requires integer values, jumps over the scan for LIMIT zero, and
  combines LIMIT plus OFFSET for loop control;
- `src/where.c:6948-6965` treats a SELECT without a FROM clause as a
  `SCAN CONSTANT ROW`; and
- `src/where.c`, `src/wherecode.c`, and `src/select.c` interleave access-path
  selection, loop construction, expression evaluation, and VDBE emission.

That coupling is explicitly rejected by ADR-0003. Modern SQLite needs a
small inspectable logical representation between binding and optimization so
that:

- relational semantics can be tested independently from access paths and
  bytecode addresses;
- the optimizer can choose table scans or rowid access without rewriting
  parser or binder objects;
- lowering can consume one immutable physical plan rather than re-running
  name or clause analysis; and
- future joins, grouping, ordering, and compounds can extend reviewed plan
  variants instead of accumulating planner flags in a syntax structure.

The current parser and binder intentionally support only:

- zero or one table source;
- optional WHERE;
- one or more result columns;
- optional LIMIT and OFFSET; and
- no joins, DISTINCT, aggregation, grouping, ordering, compounds, subqueries,
  or windows.

The logical plan must remain the smallest representation of that current
grammar. It must not pre-design multi-source schemas, statistics, costs,
physical cursors, registers, or bytecode control flow.

### Observable evaluation order

For the supported non-aggregate SELECT shape, relational cardinality alone
does not determine all SQLite behavior. Scalar functions may be
non-deterministic or have application-visible effects.

Behavioral probes against the pinned SQLite C API establish that:

- `SELECT tick(x) FROM t LIMIT 1 OFFSET 1` invokes `tick` once, only for the
  emitted row, not for the skipped row;
- `SELECT tick(x) FROM t LIMIT 0` does not invoke `tick`; and
- `SELECT tick(x) FROM t LIMIT limit_value() OFFSET 1` evaluates
  `limit_value()` once and still invokes `tick` only for the emitted row.

This matches SQLite's `selectInnerLoop()` ordering: OFFSET is checked before
result expressions are loaded. SQLite also evaluates and coerces LIMIT before
OFFSET; a zero LIMIT finishes successfully, and an invalid LIMIT aborts,
without evaluating OFFSET or initializing the source. A logical shape of
`Limit(Projection(Filter(Source)))` would permit or require projection of
rows that OFFSET discards. The initial logical shape must instead be:

```text
Projection
  -> optional Limit
       -> optional Filter
            -> Scan or SingleRow
```

The Limit operator controls whether it requests a child row, while Projection
evaluates result expressions only for rows that survive OFFSET and LIMIT.

## Decision

Add the immutable logical-plan contract under:

- `include/modern_sqlite/planner/logical_plan.hpp`; and
- `src/planner/logical_plan.cpp`.

The entry point is:

```cpp
using BuildLogicalPlanResult =
    std::expected<LogicalPlan, LogicalPlanError>;

[[nodiscard]] BuildLogicalPlanResult BuildLogicalPlan(
    BoundSelect bound_select);
```

Building a logical plan consumes one valid `BoundSelect`. On success,
`LogicalPlan` owns that bound statement and a compact postorder node arena.
On failure, no partial plan is observable. Allocation failure propagates as
`std::bad_alloc`; it is not translated into a planner error.

### Ownership boundary

`LogicalPlan` is non-copyable and nothrow movable. Its implementation is
hidden behind unique ownership. It provides const accessors for:

- the retained `BoundSelect`;
- the immutable logical-node arena;
- a node by strong ID; and
- the root node ID.

The plan does not copy SQL source, catalog snapshots, bound expressions,
function metadata, collation metadata, parameters, or result-column metadata.
Those remain owned by the retained bound statement. Logical nodes use
`BoundExpressionId`, `TableId`, and binder source metadata as stable semantic
references.

Moving a `LogicalPlan` transfers all ownership without invalidating node IDs
inside the destination. A moved-from plan is invalid, reports an empty node
span, and may be destroyed or assigned. Other accessors require a valid plan.

The plan retains no pointer to the builder, session, optimizer, lowering
state, VM, or mutable environment.

### Strong node IDs and arena

Logical nodes use an independent dense ID:

```cpp
class LogicalNodeId final {
 public:
  explicit constexpr LogicalNodeId(std::uint32_t value) noexcept;
  constexpr std::uint32_t value() const noexcept;
  constexpr auto operator<=>(const LogicalNodeId&) const noexcept = default;
};
```

`LogicalNodeId` is not convertible to or from bound-expression, catalog,
bytecode, register, cursor, or instruction IDs.

Nodes are stored in postorder. Every input ID is smaller than its consumer ID,
and the root is the final node. The current SELECT grammar produces between
two and four nodes:

1. one `LogicalSingleRowNode` or `LogicalScanNode`;
2. an optional `LogicalFilterNode`;
3. an optional `LogicalLimitNode`; and
4. one `LogicalProjectionNode`.

The fixed chain avoids parent pointers, recursive ownership, shared mutable
nodes, and graph traversal during destruction.

### Node variants

The public payload variants are:

```cpp
struct LogicalSingleRowNode {};

struct LogicalScanNode {
  BoundSourceKind source_kind;
  std::optional<TableId> table;
};

struct LogicalFilterNode {
  LogicalNodeId input;
  BoundExpressionId predicate;
};

struct LogicalLimitNode {
  LogicalNodeId input;
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset;
};

struct LogicalProjectionNode {
  LogicalNodeId input;
  std::vector<BoundExpressionId> expressions;
};
```

`LogicalNode` is a tagged union of those payloads. Stable
`LogicalNodeKindOf()` and `LogicalNodeKindName()` helpers support diagnostics
and tests without depending on variant indices.

No logical node carries:

- a B-tree root page;
- an index or rowid access choice;
- estimated cardinality or cost;
- a cursor or register number;
- a jump target;
- a bytecode instruction;
- an execution callback; or
- a copied bound expression.

Those belong to optimization, lowering, or execution.

### Single-row source

A SELECT without FROM begins with `LogicalSingleRowNode`. It produces exactly
one empty input row.

This permits:

- `SELECT 1` to project one row;
- `SELECT 1 WHERE 0` to filter that row;
- `SELECT 1 LIMIT 0` to request no input row; and
- parameter, function, and lazy-expression evaluation without inventing a
  synthetic catalog table.

The name follows relational behavior rather than SQLite's EXPLAIN wording;
it represents the same constant-row source reported by pinned SQLite.

### Logical scan

A SELECT with FROM begins with `LogicalScanNode`.

For `BoundSourceKind::kCatalogTable`, `table` contains the stable catalog
`TableId`. The logical scan means only "produce rows from this table." It does
not choose:

- a full table scan;
- INTEGER PRIMARY KEY lookup;
- index access;
- covering access;
- scan direction; or
- a physical root page.

Node 28, the basic optimizer, makes the current full-scan versus rowid-access
choice from this logical source, the filter expression, and the retained
catalog snapshot.

For `BoundSourceKind::kSchemaTable`, `table` is absent. The logical source is
the synthetic schema-table stream defined by ADR-0033. Physical planning maps
that source to the page-1 schema table and its five published fields.

Aliases and textual qualifiers are absent because binding already resolved
them. Source-column and rowid expressions continue to use the retained
bound-expression arena.

### Filter

When the bound statement has a WHERE expression, the source is wrapped in one
`LogicalFilterNode`.

The predicate is the exact `BoundExpressionId` published by the binder.
Logical planning does not:

- split conjunctions;
- fold constants;
- extract rowid constraints;
- reorder expressions;
- simplify truth tests;
- remove likelihood hints; or
- common alias-reference targets.

Those are optimizer decisions. Preserving the original bound predicate also
preserves SQLite's per-occurrence evaluation contract for result aliases and
non-deterministic expressions.

The filter retains rows for which the predicate has SQL truth. NULL and
numeric/text coercion behavior is implemented during lowering and execution
using the already bound expression semantics.

### Limit and offset

When the bound statement has LIMIT, the filtered source is wrapped in one
`LogicalLimitNode`.

The binder has already normalized `LIMIT offset, count` into:

- `limit = count`; and
- `offset = offset`.

Logical planning preserves those exact bound expression IDs.

The node contract is:

- evaluate and coerce the limit expression once during statement
  initialization;
- abort on a limit-coercion error without evaluating OFFSET or initializing
  the source;
- finish successfully for LIMIT zero without evaluating OFFSET, initializing
  the source, or evaluating result expressions;
- otherwise evaluate and coerce the optional offset expression once, before
  initializing the source;
- apply SQLite's negative-value semantics during lowering and execution;
- count only rows that survive the filter;
- discard OFFSET rows without evaluating result projections;
- stop requesting rows after the output count reaches a non-negative limit.

The logical node records semantics, not registers or jumps.

### Projection and statement root

Every plan ends in one `LogicalProjectionNode`, which is always the root.

Its expression vector copies the ordered `BoundExpressionId` from every
`BoundResultColumn`. The vector has the same size and order as
`BoundSelect::result_columns()`. Result names, declared types, and affinities
remain in the retained bound statement and are not duplicated.

Projection evaluates expressions only for rows requested from its input.
Therefore a Limit node, when present, is below Projection and can skip OFFSET
rows or all LIMIT-zero input without evaluating result expressions.

The projection node does not authorize memoization of:

- duplicate expression IDs;
- result-alias targets;
- non-deterministic function calls; or
- lazy conditional branches.

Lowering emits each result occurrence according to binder semantics.

### Construction and validation

`BuildLogicalPlan()` performs no semantic lookup. It:

1. rejects an invalid or moved-from `BoundSelect`;
2. reserves the exact node count;
3. appends the source node;
4. appends Filter when WHERE exists;
5. appends Limit when LIMIT exists;
6. copies result expression IDs into Projection;
7. appends Projection as the root; and
8. validates the completed immutable shape before publication.

Publication invariants are:

- the retained bound statement is valid;
- the node count is from two through four;
- the first node is SingleRow exactly when
  `BoundSelect::table_source()` is null;
- otherwise the first node is Scan and its source kind and optional table ID
  exactly equal the retained `BoundTableSource`;
- a catalog-table source and scan have a table ID;
- a schema-table source and scan have no table ID;
- every node after the first consumes the immediately preceding node, so the
  root reaches every arena node exactly once in
  Source -> Filter? -> Limit? -> Projection order;
- Filter exists exactly when the bound statement has WHERE and references the
  same expression ID;
- Limit exists exactly when the bound statement has LIMIT and references the
  same normalized limit and offset IDs;
- Projection is final, is the root, and references every bound result
  expression exactly once in result order; and
- every referenced bound expression ID is within the retained expression
  arena.

Invariant failure returns a typed internal error rather than a
success-shaped fallback or assertion-only failure.

### Errors

The error surface is intentionally small:

```cpp
enum class LogicalPlanErrorCode : std::uint8_t {
  kInvalidInput,
  kInternalInvariant,
};

struct LogicalPlanError {
  LogicalPlanErrorCode code;
  std::string detail;

  ErrorCode base_error_code() const noexcept;
};
```

Mappings are:

- invalid or moved-from bound input -> `ErrorCode::kMisuse`; and
- impossible retained-plan invariant -> `ErrorCode::kInternal`.

All SQL-facing semantic errors have already been produced by parsing or
binding. Logical planning does not relabel missing names, unsupported syntax,
invalid literals, missing collations, or limits as planner errors.

### Layering

The public logical-plan header may include only:

- standard-library headers;
- `modern_sqlite/base/result.hpp`; and
- `modern_sqlite/binder/bound_select.hpp`.

The implementation may depend on the same logical-plan, binder, and catalog
contracts. It must not include:

- syntax parser implementation;
- bytecode;
- VM;
- B-tree, pager, cache, VFS, or record-codec internals;
- optimizer or physical-plan types;
- lowering; or
- session/API types.

The binder remains unaware of logical planning. Bytecode and VM remain
unaware of both logical and physical plans.

### Performance and allocation contract

The node is not marked performance-sensitive in the project graph, but plan
construction is still on the prepare path.

The implementation therefore:

- moves the bound statement without copying its owned state;
- reserves exactly two through four logical nodes;
- performs no name, catalog, function, or collation lookup;
- performs no recursion or hashing;
- allocates only the plan implementation, node arena, and projection
  expression vector;
- copies only strong expression IDs into Projection; and
- provides allocation-free immutable reads after publication.

No benchmark baseline is required for this node. A deterministic allocation
test guards against accidental expression or metadata copying.

## Verification

Development follows ADR-0005:

1. accept this ADR after independent design review;
2. add the public contract and red-first tests;
3. confirm the expected missing-implementation failure;
4. implement the smallest builder;
5. run focused and complete validation; and
6. obtain independent final code review before commit.

Tests cover:

- strong node-ID type separation and stable kind names;
- non-copyable, nothrow-movable ownership;
- SingleRow plus Projection for SELECT without FROM;
- catalog-table and schema-table Scan identity;
- optional Filter insertion and exact predicate identity;
- normalized Limit and Offset identity;
- `Projection(Limit(Filter(Source)))` postorder and root shape;
- projection expression order and result metadata retention;
- WHERE alias-reference identity without memoization;
- catalog and SQL-source lifetime through the retained `BoundSelect`;
- moved-from bound-input rejection;
- pinned-SQLite differential probes proving that LIMIT zero and invalid LIMIT
  bypass a side-effecting OFFSET expression and source initialization;
- allocation-free published access;
- deterministic allocation counts and allocation failure at each ownership
  boundary;
- exact public-header dependency layering;
- formatting, graph, and whitespace validation;
- Debug and Release builds;
- ASan/UBSan and TSan; and
- GCC and Clang warning-clean builds.

## Consequences

- The optimizer receives an inspectable relational statement rather than
  syntax or bytecode.
- Observable SQLite evaluation order for OFFSET, LIMIT zero, and result
  expressions is explicit in the initial plan shape.
- The current plan is a linear single-source tree; joins and other operators
  require reviewed variants later.
- Bound expression and result metadata have one owner and are not duplicated.
- Physical access choice, cost, root pages, cursor IDs, registers, and jumps
  remain outside the logical layer.
- Lowering can later traverse one immutable physical plan without repeating
  binding or clause-order analysis.
