# ADR-0035: Deterministic Basic Read Optimization

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0034 publishes immutable logical SELECT plans containing statement
semantics but no access path. The next dependency-graph node must choose the
first physical read strategy without emitting bytecode or introducing the
index and join machinery reserved for later work.

The supported SQL remains deliberately small:

- zero or one source;
- optional WHERE;
- optional LIMIT and OFFSET;
- one projection;
- ordinary rowid tables, WITHOUT ROWID tables, and the synthetic
  `sqlite_schema` source; and
- no joins, ordering, grouping, aggregation, DISTINCT, compounds,
  subqueries, windows, or index hints.

The module-graph deliverable is:

> Deterministic table-scan and rowid access selection with explicit cost
> data.

Its acceptance matrix requires:

- full scans;
- rowid equality;
- impossible predicates; and
- stable explain output.

Pinned SQLite 3.54.0 interleaves these decisions with WHERE analysis and VDBE
generation:

- `src/where.c:4139-4175` constructs the INTEGER PRIMARY KEY full-scan
  candidate and estimates its work from the table row estimate;
- `src/where.c:6386-6400` recognizes `rowid = expression` and
  `rowid IS expression` as one-row INTEGER PRIMARY KEY access;
- `src/wherecode.c:145-201` reports `SCAN`, `SEARCH`, and
  `USING INTEGER PRIMARY KEY (rowid=?)`;
- `src/where.c:6999-7054` emits a statement guard for deterministic false
  WHERE terms so no source code runs; and
- `src/build.c:1368-1374` uses 1,048,576 rows as the default estimate when no
  statistics are available.

Pinned behavioral and EXPLAIN QUERY PLAN probes additionally establish that:

- `SELECT 1` reports `SCAN CONSTANT ROW`;
- an unconstrained rowid table reports `SCAN t`;
- `rowid = ?`, `rowid IS ?`, `? = rowid`, and an INTEGER PRIMARY KEY alias
  all report `SEARCH t USING INTEGER PRIMARY KEY (rowid=?)`;
- rowid equality remains eligible through whole-predicate `likely(...)`,
  explicit operand COLLATE, and a result-alias reference;
- unary plus deliberately disables rowid access;
- `rowid = source_column` remains a scan because the lookup key depends on
  the current source row;
- NULL and non-deterministic source-independent rowid keys still use rowid
  access;
- rowid access is selected with a residual conjunct on either side;
- a deterministic false conjunct suppresses rowid access and source
  execution;
- `sqlite_schema` supports INTEGER PRIMARY KEY rowid access;
- a WITHOUT ROWID primary-key equality uses an index search in SQLite, which
  is outside this node and remains a full scan here; and
- SQLite's EQP text still says `SCAN t` for a false predicate even though its
  generated statement bypasses the scan at runtime.

Modern SQLite needs a stable physical-plan boundary that makes the selected
access path and its cost inspectable before lowering. It must preserve the
observable expression-evaluation rules already fixed by ADR-0033 and
ADR-0034:

- LIMIT and OFFSET initialize before any WHERE guard or source work;
- source-independent deterministic WHERE conjuncts for a table source execute
  once in source order before a rowid key or source is initialized;
- for a no-FROM SingleRow source, every unknown WHERE conjunct executes once
  in original order before the synthetic row, including non-deterministic
  functions;
- LIMIT zero executes no WHERE guard;
- a selected rowid key is evaluated once after all guards pass and before
  residual predicates;
- the consumed rowid equality is not evaluated a second time;
- false or NULL guards bypass rowid-key, source, residual, and projection
  evaluation;
- a compile-time false conjunct preserves earlier statement guards but skips
  later guards and table-source residual or key terms;
- residual conjuncts preserve source order; and
- LIMIT/OFFSET remain below Projection and are not folded into access choice.

This node must not grow into a general optimizer. In particular, it must not
choose ordinary indexes, covering indexes, WITHOUT ROWID primary-key
searches, rowid ranges, OR-union paths, automatic indexes, join order, sort
order, or bytecode registers.

## Decision

Add the physical-plan and optimizer contract under:

- `include/modern_sqlite/optimizer/physical_plan.hpp`; and
- `src/optimizer/physical_plan.cpp`.

The entry point is:

```cpp
using OptimizeLogicalPlanResult =
    std::expected<PhysicalPlan, OptimizerError>;

[[nodiscard]] OptimizeLogicalPlanResult OptimizeLogicalPlan(
    LogicalPlan logical_plan);
```

Optimization consumes one valid `LogicalPlan`. On success, `PhysicalPlan`
owns that logical plan, a compact postorder physical-node arena, fixed-capacity
access-path candidate data, and the selected candidate index. On failure, no
partial plan is observable. `std::bad_alloc` propagates.

### Ownership boundary

`PhysicalPlan` is non-copyable and nothrow movable. A hidden uniquely owned
implementation retains:

- the complete `LogicalPlan`;
- physical nodes;
- one or two access-path candidates; and
- the selected candidate index.

The optimizer does not copy:

- SQL source;
- catalog snapshots;
- bound expressions;
- source-column metadata;
- functions, collations, or parameters;
- result metadata; or
- logical projection expression arrays.

Physical nodes refer to retained `BoundExpressionId` and `LogicalNodeId`
values. Moving a plan preserves those IDs in the destination. A moved-from
plan is invalid, reports empty node and candidate spans, and may be destroyed
or assigned. Other accessors require a valid plan.

The plan retains no session, VM, pager, B-tree cursor, register, instruction,
or mutable optimizer object.

### Strong physical node IDs

```cpp
class PhysicalNodeId final {
 public:
  explicit constexpr PhysicalNodeId(std::uint32_t value) noexcept;
  constexpr std::uint32_t value() const noexcept;
  constexpr auto operator<=>(const PhysicalNodeId&) const noexcept = default;
};
```

`PhysicalNodeId` is distinct from logical, bound, catalog, bytecode, cursor,
register, and instruction IDs.

Physical nodes are stored in postorder. Every consumer references the
immediately preceding node, and the root is final. The current grammar
produces:

1. one physical access leaf;
2. an optional statement Guard;
3. an optional residual Filter;
4. an optional Limit; and
5. one Projection root.

The physical chain therefore remains linear and contains two through five
nodes.

### Physical access leaves

```cpp
enum class PhysicalAccessKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
};

[[nodiscard]] std::string_view PhysicalAccessKindName(
    PhysicalAccessKind kind) noexcept;
```

The supported leaf variants are:

```cpp
struct PhysicalSingleRowNode {};

struct PhysicalEmptyNode {};

struct PhysicalTableScanNode {
  BoundSourceKind source_kind;
  std::optional<TableId> table;
  RootPageId root_page;
};

struct PhysicalRowIdLookupNode {
  BoundSourceKind source_kind;
  std::optional<TableId> table;
  RootPageId root_page;
  BoundExpressionId key;
};
```

`PhysicalSingleRowNode` implements the logical no-FROM source.

`PhysicalEmptyNode` produces no rows and opens no source. It is selected only
when the WHERE conjunction contains a compile-time known false or NULL term.
This names SQLite's false-WHERE runtime bypass as an explicit physical
decision; its explain text intentionally differs from SQLite's still-present
`SCAN` line.

`PhysicalTableScanNode` represents:

- an ordinary rowid-table scan at `CatalogTable::root_page`;
- a WITHOUT ROWID table scan at `CatalogTable::root_page`; or
- a schema-table scan at root page 1.

It does not select an ordinary or primary-key index.

`PhysicalRowIdLookupNode` represents one exact seek in an ordinary rowid
table or `sqlite_schema`. Its key is evaluated once. A WITHOUT ROWID table
never produces this leaf.

### Physical relational nodes

```cpp
struct PhysicalGuardNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates;
};

struct PhysicalFilterNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates;
};

struct PhysicalLimitNode {
  PhysicalNodeId input;
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset;
};

struct PhysicalProjectionNode {
  PhysicalNodeId input;
  LogicalNodeId logical_projection;
};
```

A statement Guard stores one or more source-independent predicate IDs in
stable left-to-right order. For a table source, every Guard predicate is
deterministic. For a no-FROM SingleRow source, every unknown WHERE conjunct is
a Guard predicate regardless of determinism because SQLite evaluates the
single predicate stream once in original order. Guard runs once when its
parent first requests input, before calling its child access node. False or
NULL returns end-of-input without initializing the child. Errors propagate.

A residual Filter stores one or more conjunct IDs in stable left-to-right
order. Lowering evaluates each occurrence once and rejects the row when a
predicate is false or NULL.

Limit copies the already normalized bound IDs and preserves ADR-0034's
one-time LIMIT-before-OFFSET initialization contract.

Projection references the retained logical projection node rather than
copying its result expression vector.

The published node wrapper is:

```cpp
using PhysicalNodePayload =
    std::variant<PhysicalSingleRowNode, PhysicalEmptyNode,
                 PhysicalTableScanNode, PhysicalRowIdLookupNode,
                 PhysicalGuardNode, PhysicalFilterNode,
                 PhysicalLimitNode, PhysicalProjectionNode>;

struct PhysicalNode {
  PhysicalNodePayload payload;
};

enum class PhysicalNodeKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
  kGuard,
  kFilter,
  kLimit,
  kProjection,
};

[[nodiscard]] PhysicalNodeKind PhysicalNodeKindOf(
    const PhysicalNode& node) noexcept;
[[nodiscard]] std::string_view PhysicalNodeKindName(
    PhysicalNodeKind kind) noexcept;
```

### WHERE conjunction analysis

Before testing for `BoundBinaryOperation::kLogicalAnd`, conjunction analysis
follows transparent alias, COLLATE, and likelihood wrappers. It recursively
flattens the resulting AND tree while preserving left-to-right leaf order and
per-occurrence evaluation. Every other expression, including OR, remains one
predicate occurrence.

Compile-time truth analysis is deliberately narrow. It recognizes:

- materialized bound literals through `EvaluateSqlTruth`;
- transparent alias, COLLATE, and likelihood wrappers;
- logical NOT when its operand is known;
- AND/OR when the required operands are known; and
- bound truth tests when their operand is known.

For AND, one known false operand is sufficient to prove that conjunct false
even when the other operand is unknown.

For a table source, flattened terms are classified in source order:

1. a known true term is removed;
2. a source-independent deterministic unknown term is appended to the
   statement Guard;
3. a known false or NULL term selects `PhysicalEmptyNode`, preserves guards
   accumulated before that term, and discards all later terms plus all
   source-dependent/non-deterministic terms; and
4. every other unknown term remains eligible for rowid extraction or
   residual filtering.

For a no-FROM SingleRow source:

1. a known true term is removed;
2. every unknown term is appended to Guard in source order, regardless of
   function determinism; and
3. a known false or NULL term selects `PhysicalEmptyNode`, preserves every
   preceding unknown Guard term, and discards later terms.

This no-FROM rule is an amendment recorded by ADR-0036. It preserves cases
such as `random() > 0 AND 0`, where pinned SQLite evaluates the first
non-deterministic conjunct before the known-false term even though no table
row exists.

This ordering preserves both:

- `rowid = random() AND 0`, where the random key never executes; and
- `abs(?) = 0 AND 0`, where `abs(?) = 0` still executes once before the known
  false term can end the statement.

An expression is deterministic when every scalar call in its retained bound
DAG references `BoundScalarFunction::deterministic == true` and every child is
deterministic. Literals, parameters, and built-in operators are deterministic.
Source independence and determinism are separate requirements. Determinism is
required for table-source guards, but not for the no-FROM SingleRow predicate
stream.

The optimizer performs no other constant folding, comparison evaluation,
contradiction solving, or algebraic rewrite.

### Rowid equality recognition

A conjunct is eligible for rowid access only when:

1. after transparent alias, COLLATE, and likelihood wrappers, it is a bound
   comparison;
2. the comparison is SQL `=` or `IS`;
3. exactly one side denotes the source rowid;
4. the other side is source-independent; and
5. the source is an ordinary rowid table or `sqlite_schema`.

A rowid reference is:

- `BoundRowIdExpression`; or
- a bound source column whose catalog column equals the table's
  `rowid_alias`.

When deciding whether either comparison operand denotes rowid, the optimizer
independently follows alias and COLLATE wrappers on that operand. It does not
follow operand-level likelihood wrappers: ADR-0033 gives likelihood
expressions NONE affinity, so replacing `likely(rowid) = '1'` with a rowid
seek would change comparison coercion and results. Likelihood remains
transparent only when it wraps the whole comparison or conjunction as a
planner hint.

The opposite operand's original `BoundExpressionId` is retained as the lookup
key so its complete runtime semantics remain intact.

Unary plus and every other unary wrapper are not transparent on a candidate
rowid operand. This preserves SQLite's explicit unary-plus optimization
barrier.

An expression is source-independent when its complete bound-expression DAG
contains no source-column or rowid occurrence. Literals, parameters, scalar
functions, comparisons, arithmetic, lazy expressions, aliases, collations,
and likelihood wrappers remain eligible when all children are
source-independent. Function determinism is not required: pinned SQLite
chooses a rowid seek for `rowid = random()` and evaluates the key once.

The first eligible conjunct in source order supplies the lookup key. It is
removed from residual predicates. Every remaining unknown conjunct is
retained in the physical Filter. This supports direct equality and arbitrary
wrapped/nested AND lists without rebuilding bound expressions.

If no eligible term exists, the source uses a full scan and all unknown
conjuncts remain residual predicates.

### Explicit access-path costs

The public cost contract is:

```cpp
struct AccessPathCost {
  std::uint64_t estimated_input_rows;
  std::uint64_t estimated_output_rows;
  std::uint64_t work_units;
};

struct AccessPathCandidate {
  PhysicalAccessKind kind;
  AccessPathCost cost;
};
```

Costs are deterministic comparison-equivalent work estimates, not elapsed
time or a claim that distinct storage operations have identical latency.

Source cardinality is:

- `CatalogTable::statistics.estimated_rows` when present;
- otherwise 1,048,576 rows, matching SQLite's default;
- 1 for SingleRow; and
- 0 for Empty.

Access costs are:

- Empty: input 0, output 0, work 0;
- SingleRow: input 1, output 1, work 1;
- TableScan: input `N`, output `N`, work `max(N, 1)`; and
- RowIdLookup: input `N`, output `min(N, 1)`, work
  `bit_width(max(N, 1))`.

The candidate span contains:

- one Empty or SingleRow candidate for those sources;
- one TableScan candidate when no rowid equality is eligible; or
- TableScan followed by RowIdLookup when rowid equality is eligible.

The lowest `work_units` wins. Lower estimated output rows break a cost tie.
RowIdLookup wins any remaining tie. Candidate order and tie-breaking are
stable public behavior.

This first cost model intentionally excludes residual-expression CPU,
cache state, row width, page size, and likelihood hints. There is no competing
ordinary index for those estimates to select yet, so invented precision would
not improve the current decision. Later optimizer ADRs may version and extend
the candidate model.

### Published physical-plan API

```cpp
class PhysicalPlan final {
 public:
  PhysicalPlan(const PhysicalPlan&) = delete;
  PhysicalPlan& operator=(const PhysicalPlan&) = delete;
  PhysicalPlan(PhysicalPlan&&) noexcept;
  PhysicalPlan& operator=(PhysicalPlan&&) noexcept;
  ~PhysicalPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const LogicalPlan& logical_plan() const noexcept;
  [[nodiscard]] std::span<const PhysicalNode> nodes() const noexcept;
  [[nodiscard]] const PhysicalNode& node(PhysicalNodeId id) const noexcept;
  [[nodiscard]] PhysicalNodeId root() const noexcept;
  [[nodiscard]] std::span<const AccessPathCandidate> candidates()
      const noexcept;
  [[nodiscard]] std::size_t selected_candidate_index() const noexcept;
  [[nodiscard]] const AccessPathCandidate& selected_candidate()
      const noexcept;
};
```

For a moved-from plan:

- `valid()` is false;
- `nodes()` and `candidates()` are empty; and
- destruction and move assignment are valid.

`logical_plan()`, `node()`, `root()`, `selected_candidate_index()`,
`selected_candidate()`, and `ExplainPhysicalPlan()` require a valid plan.
Node IDs and candidate references remain stable for the lifetime of a valid
plan.

### Stable explain output

```cpp
[[nodiscard]] std::string ExplainPhysicalPlan(const PhysicalPlan& plan);
```

The current output is one stable line:

```text
SCAN CONSTANT ROW
EMPTY RESULT
SCAN "<escaped-canonical-table-name>"
SCAN "sqlite_schema"
SEARCH "<escaped-canonical-table-name>" USING INTEGER PRIMARY KEY (rowid=?)
SEARCH "sqlite_schema" USING INTEGER PRIMARY KEY (rowid=?)
```

Canonical catalog names are used rather than source aliases because binding
does not retain aliases as physical identity. Every identifier is enclosed
in double quotes. Escaping is byte-stable:

- `"` becomes `\"`;
- `\` becomes `\\`;
- printable ASCII bytes from space through `~`, other than `"` and `\`,
  remain unchanged; and
- every other byte, including controls, DEL, valid non-ASCII UTF-8, and
  invalid UTF-8, becomes uppercase `\xHH`.

An empty name therefore renders as `""`, and no legal catalog name can add
another output line. Cost data remains structured and is not embedded in
explain text.

Explain allocates its returned string. Plan construction and immutable plan
accessors do not allocate after publication.

### Construction and validation

`OptimizeLogicalPlan()`:

1. rejects an invalid or moved-from logical plan;
2. derives the logical source and catalog identity;
3. flattens the optional WHERE conjunction;
4. removes known true terms, extracts source-appropriate statement guards, or
   selects Empty for known false/NULL while preserving prior guards;
5. derives full-scan and optional rowid candidates from remaining terms;
6. selects the deterministic minimum-cost candidate;
7. appends the selected physical leaf;
8. appends Guard when statement predicates remain;
9. appends residual Filter when row predicates remain;
10. appends the exact logical Limit when present;
11. appends Projection referencing the logical projection root; and
12. validates the completed immutable shape before publication.

Publication invariants are:

- the retained logical plan is valid;
- the physical arena contains two through five nodes;
- the root is final;
- every consumer input is the immediately preceding node;
- exactly one access leaf is first;
- the leaf kind equals the selected candidate kind;
- candidate count, order, costs, and selected index are valid;
- scan/lookup source kind, optional table ID, and root page exactly match the
  retained logical source and catalog;
- a rowid lookup key is in range, source-independent, and belongs to an
  eligible rowid source;
- statement Guard vectors are non-empty, source-independent, source-ordered,
  and every ID is in range;
- Guard predicates are deterministic for table sources, while a SingleRow
  source may retain non-deterministic predicates;
- residual predicate vectors are non-empty and every ID is in range;
- Guard, when present, is immediately above the access leaf and below any
  residual Filter, Limit, and Projection;
- Limit exists exactly when the logical plan has Limit and preserves the same
  limit and offset IDs;
- Projection is final and references the retained logical projection root;
  and
- no physical node contains bytecode, cursor, register, or mutable execution
  state.

Invariant failure returns a typed internal error.

### Errors

```cpp
enum class OptimizerErrorCode : std::uint8_t {
  kInvalidInput,
  kInternalInvariant,
};

struct OptimizerError {
  OptimizerErrorCode code;
  std::string detail;

  ErrorCode base_error_code() const noexcept;
};
```

Mappings are:

- invalid or moved-from logical input -> `ErrorCode::kMisuse`; and
- impossible retained-plan invariant -> `ErrorCode::kInternal`.

SQL-facing semantic errors remain parser or binder errors. The optimizer does
not convert allocation failure, invalid LIMIT values, function failures, or
runtime key coercion into optimizer errors.

### Layering

The public optimizer header may include only:

- standard-library headers;
- `modern_sqlite/base/result.hpp`; and
- `modern_sqlite/planner/logical_plan.hpp`.

The implementation may use optimizer, logical-plan, binder, catalog, and SQL
value contracts. It must not include:

- syntax parser implementation;
- bytecode;
- lowering;
- VM;
- pager, B-tree, cache, VFS, or record-codec internals;
- session or API types; or
- transaction state.

Logical planning and binding remain unaware of physical optimization.
Lowering consumes the immutable physical contract but the optimizer remains
unaware of bytecode addresses.

### Allocation and performance contract

Optimization is on the prepare hot path and this project node is marked
performance-sensitive.

The implementation:

- moves the logical plan without copying retained state;
- reserves the exact physical node count;
- uses fixed-capacity storage for at most two access candidates;
- performs direct catalog access by stable ID, not name lookup;
- recursively inspects the already bound expression DAG without cloning it;
- reuses the flattened conjunct vector as one published predicate vector and
  allocates a second vector only when both statement guards and residual
  predicates are present;
- allocates the physical implementation and node arena;
- performs no parser, binder, collation, function, or catalog-name work; and
- provides allocation-free immutable reads after publication.

Successful construction therefore performs:

- two allocations without WHERE; or
- three allocations when WHERE publishes at most one predicate vector; or
- four allocations when both Guard and residual Filter vectors are
  published.

OOM tests inject every construction boundary. Allocation tests verify exact
counts and allocation-free access. Explain allocation is measured separately.

A Release benchmark compares:

- Modern SQLite parse, bind, logical-plan construction, and optimization; and
- pinned SQLite 3.54.0 `EXPLAIN QUERY PLAN` prepare.

Both sides use the same preloaded schema, SQL, compiler optimization level,
warm process state, and full-scan, direct-rowid, residual-rowid, and
false-predicate cases. Schema loading, database open, result stepping,
explain-string formatting, and destruction are outside the timed region.

Because pinned SQLite also emits an EQP VDBE while Modern SQLite stops at the
physical plan, the initial gate remains a severe-regression guard rather than
a parity claim: every case must remain within 10 times pinned SQLite's median
and the report must include provenance, per-case medians, and allocation
counts.

The reviewed implementation's final Apple Clang 21 arm64 benchmark produced:

| Workload | Modern median | SQLite median | Ratio | Modern allocations | SQLite allocations |
|---|---:|---:|---:|---:|---:|
| Full scan | 780.80 ns | 731.42 ns | 1.067515 | 19 | 18 |
| Direct rowid | 999.78 ns | 778.31 ns | 1.284549 | 30 | 20 |
| Residual rowid | 1363.53 ns | 1084.79 ns | 1.256955 | 36 | 27 |
| False predicate | 778.73 ns | 715.09 ns | 1.088992 | 24 | 19 |

The maximum ratio is 1.284549, so every workload passes the 10x
severe-regression gate. The source, raw result, and provenance records are the
session artifacts `optimizer_benchmark.cpp`,
`optimizer_benchmark-results.json`, and
`optimizer_benchmark-provenance.json`.

## Verification

Development follows ADR-0005:

1. accept this ADR after independent design review;
2. add the public contract and red-first tests;
3. confirm the expected missing-implementation failure;
4. implement the smallest optimizer;
5. build and validate the pinned benchmark;
6. run focused and complete validation; and
7. obtain independent final code review before commit.

Tests cover:

- strong physical-ID type separation and stable kind names;
- non-copyable, nothrow-movable ownership;
- SingleRow and Empty access;
- rowid-table, schema-table, and WITHOUT ROWID scans;
- direct, reversed, `IS`, INTEGER PRIMARY KEY alias, result-alias,
  whole-predicate likelihood, COLLATE, NULL-key, parameter, and
  non-deterministic rowid equality;
- operand-level likelihood rejection, including the text-key affinity case;
- unary-plus and source-dependent-key rejection;
- residual conjunctions before and after the rowid term;
- deterministic table-source statement guards before key/source
  initialization, including empty-table behavior, errors, source-order
  short-circuiting, and LIMIT-zero suppression;
- no-FROM non-deterministic predicate order, including preservation before a
  later known-false conjunct;
- consumed-term removal and stable residual order;
- false/NULL bypass, true-term removal, prior-guard preservation, and false
  conjunctions that suppress non-deterministic rowid keys;
- conjunction splitting through alias, COLLATE, and likelihood wrappers;
- exact root-page and source identity;
- explicit candidate order, cost values, tie-breaking, and selected index;
- exact Limit and Projection retention;
- stable one-line explain output for ordinary, empty, quoted, backslash,
  control-byte, valid non-ASCII, and invalid UTF-8 names;
- catalog, SQL-source, and bound-metadata lifetime through the retained
  logical plan;
- moved-from logical-input rejection;
- exact two-, three-, and four-allocation construction paths;
- allocation failure at every ownership boundary;
- allocation-free published access;
- optimizer layering;
- benchmark report shape, provenance, and severe-regression guard;
- Debug and Release builds;
- ASan/UBSan and TSan;
- clang-tidy and warnings-as-errors on GCC and Clang;
- formatting, graph, layering, and whitespace checks; and
- independent final code review.

## Consequences

- Lowering receives an immutable, inspectable physical access decision.
- Rowid equality is evaluated once and is not redundantly retained as a
  residual predicate.
- Source-independent guards execute once before access while LIMIT zero still
  suppresses them; table guards require determinism, while no-FROM guards
  preserve every unknown conjunct.
- Deterministic false predicates become an explicit Empty access path.
- Cost data is structured, stable, and intentionally simple enough to audit.
- Ordinary indexes, WITHOUT ROWID key search, ranges, joins, and ordering
  remain deferred rather than partially implemented.
- Explain output is stable without coupling the optimizer to bytecode.
- The complete SQL, catalog, bound expression, logical node, physical node,
  and access-cost chain remains available for diagnostics and future
  AI-native plan analysis.
