# ADR-0060: SQLite-Aligned Aggregate Binding

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0057 package 8 binds aggregate queries after package 6 added syntax and
package 7 added immutable aggregate descriptors/state. The current binder
already provides:

- one ordinary SELECT source per core plus VALUES and compounds;
- immutable bound expressions, result metadata, aliases, parameters,
  collations, scalar functions, ORDER BY, LIMIT, and DISTINCT;
- source-column-before-alias lookup in ordinary alias-enabled scopes;
- structural syntax and bound-expression comparison; and
- explicit downstream planning/lowering contracts.

Aggregate binding must now resolve calls and clauses without starting
planning, bytecode, grouping, or execution. It must preserve SQLite's error
order and representative-row information while keeping the bound result
immutable and independent from runtime descriptor pointers.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/resolve.c:1073-1410`, for scalar/aggregate function resolution,
  modifier errors, nested-aggregate rejection, collation, and aggregate-query
  flags;
- `src/resolve.c:1826-2115`, for result-first resolution, aggregate-query
  status, HAVING legality, aliases, ORDER BY/GROUP BY ordinals, and clause
  ordering;
- `src/expr.c:2836-2889`, for HAVING-to-WHERE constant/group-term matching;
- `src/expr.c:7396-7660`, for retained aggregate columns, invocation
  deduplication, and local-order metadata; and
- `src/select.c:7082-7152` and `src/select.c:8529-8570`, for HAVING transfer
  and aggregate analysis order.

Pinned behavior probes additionally establish:

- source columns precede result aliases in GROUP BY and HAVING;
- HAVING does not make a nonaggregate query aggregate, even when HAVING
  itself contains an aggregate;
- an aggregate appearing only in ORDER BY is misuse unless GROUP BY or a
  result aggregate already established aggregate-query status;
- WHERE aliases that expand to an aggregate remain aggregate misuse;
- GROUP BY recognizes transparent parentheses, signed integer forms, and an
  outer COLLATE as result ordinals only when the unsigned literal magnitude
  is at most `INT32_MAX`;
- larger integer spellings are ordinary expressions;
- nested aggregate calls are misuse;
- scalar FILTER is rejected before its predicate is resolved;
- nonempty scalar local ordering is rejected after ordinary arguments bind
  but before local order terms bind;
- zero-argument local ordering is discarded before semantic resolution; and
- `count(*)` is a zero-argument aggregate call, while other `f(*)` forms use
  effective arity zero for ordinary function resolution.

## Decision

### 1. Add immutable aggregate identities and programs

The binder adds:

```cpp
using BoundAggregateFunctionId =
    BoundId<struct BoundAggregateFunctionIdTag>;
using BoundAggregateInvocationId =
    BoundId<struct BoundAggregateInvocationIdTag>;

enum class BoundAggregateExtremumKind : std::uint8_t {
  kNone,
  kMinimum,
  kMaximum,
};

struct BoundAggregateFunction {
  std::string name;
  bool deterministic = false;
  bool uses_collation = false;
  BoundAggregateExtremumKind extremum =
      BoundAggregateExtremumKind::kNone;
};

struct BoundAggregateCallExpression {
  BoundAggregateInvocationId invocation;
};

struct BoundAggregateInvocation {
  SourceSpan span;
  BoundAggregateFunctionId function;
  BoundCollationId collation;
  std::vector<BoundExpressionId> arguments;
  bool distinct = false;
  std::vector<BoundOrderingTerm> order_by;
  std::optional<BoundExpressionId> filter;
};

struct BoundGroupingTerm {
  BoundExpressionId expression;
  BoundCollationId collation;
  std::optional<std::size_t> result_column;
};

struct BoundAggregateProgram {
  std::vector<BoundAggregateInvocationId> invocations;
  std::vector<BoundGroupingTerm> group_by;
  std::optional<BoundExpressionId> having;
  std::vector<BoundExpressionId> source_leaves;
  std::vector<BoundExpressionId> representative_leaves;
};
```

`BoundSelectCore` gains an optional `BoundAggregateProgram`.
`BoundSelect` publishes immutable aggregate-function and invocation tables.
IDs are dense and statement local. Bound data owns names and metadata and
retains no `AggregateFunction*`, registry span, syntax node, planner object,
register, cursor, sorter, relation, or VM state.

`BoundExpressionProperties` gains `contains_aggregate`. The flag propagates
through aliases, unary/binary expressions, collations, scalar/lazy calls, and
truth/comparison nodes. A bound aggregate-call reference sets it.

### 2. Preserve clause resolution and error order

For each ordinary SELECT core:

1. bind the source;
2. bind result expressions left to right with aggregates allowed but result
   aliases hidden;
3. establish aggregate-query status from nonempty GROUP BY syntax or a
   result expression whose properties contain an aggregate;
4. publish result aliases;
5. reject HAVING unless aggregate-query status already exists;
6. bind HAVING with aggregates allowed and source columns before aliases;
7. bind WHERE with aggregates disabled and source columns before aliases;
8. bind singleton top-level ORDER BY with aggregates allowed only when the
   core is already aggregate;
9. bind GROUP BY with aggregates disabled and source columns before aliases;
10. transfer eligible HAVING leaves into WHERE; and
11. finalize invocation deduplication and aggregate leaf analysis.

This order is independent from invocation-table order. Compound ORDER BY
retains ADR-0056 speculative output matching and never establishes arm
aggregate status or creates aggregate invocations.

An aggregate reached while aggregates are disabled, directly or through a
result alias, reports `kAggregateMisuse`. A direct illegal aggregate still
binds its ordinary arguments first so a later argument error wins; modifiers
remain unresolved. HAVING on a nonaggregate query reports
`kHavingWithoutAggregate`. GROUP BY resolves an outer aggregate far enough to
bind its children, then reports `kAggregateMisuse` with the
SQLite-compatible clause detail.

### 3. Resolve scalar and aggregate overloads by effective arity

A function call first determines effective ordinary arguments:

- the bare-star production has effective arity zero and no bound argument;
- omitted or explicit ALL keeps the parsed ordinary argument count; and
- ordinary arguments bind left to right.

Resolution preserves existing lazy special forms and registry override
precedence:

1. a matching scalar registry descriptor selects eager scalar binding;
2. otherwise matching binder-owned `coalesce`, `ifnull`, `iif`/`if`,
   `likely`, `unlikely`, or `likelihood` syntax selects its existing lazy or
   hint binding;
3. otherwise a matching aggregate descriptor selects aggregate binding;
4. a known scalar, special-form, or aggregate name with no matching arity
   records provisional `kWrongFunctionArity`; and
5. an unknown name records provisional `kNoSuchFunction`.

This preserves scalar MIN/MAX at arity two or greater and aggregate MIN/MAX at
arity one. COUNT resolves at arity zero or one. GROUP_CONCAT resolves at one
or two, and STRING_AGG at two.

Function argument limits apply to effective ordinary arguments.

Function-level lookup errors are provisional. Ordinary arguments still bind
left to right. A later argument error replaces the provisional diagnostic;
otherwise the saved lookup error is returned. Deferred scalar FILTER
predicates and scalar local-order terms remain unresolved.

### 4. Enforce aggregate legality and modifier ordering

Aggregate arguments, FILTER predicates, and local ordering terms bind with
aggregates disabled, preventing nested calls.

Scalar DISTINCT remains accepted and semantically ignored.

Aggregate DISTINCT requires exactly one ordinary argument. The cardinality
check occurs only after ordinary arguments, aggregate local ordering, and
FILTER bind successfully, so child expression errors retain SQLite
precedence. Other aggregate arity errors retain ordinary wrong-arity
diagnostics.

For scalar calls:

1. resolve function and arity;
2. bind ordinary arguments;
3. reject FILTER without resolving its predicate; and
4. reject nonempty local ordering without resolving its terms.

Zero-argument local ordering is discarded before scalar or aggregate
semantic resolution, so unresolved names inside it produce no error.

For aggregate calls, local ordering terms use the ordinary ORDER BY term
shape but never perform result-ordinal or output-expression matching. Their
expressions bind in the enclosing source-before-result-alias scope with
aggregates disabled. MIN/MAX terms are resolved and stored for structural
identity, but later execution does not evaluate them.

FILTER binds after local ordering, in the call's current source/alias scope,
with aggregates disabled.

### 5. Separate invocation comparison collation from result collation

Every invocation records a comparison collation ID. A non-DISTINCT aggregate
whose descriptor does not use collation records BINARY without requiring an
ordinary argument's explicit collation. A collation-using aggregate selects
and requires the first ordinary argument collation, falling back to BINARY.
Valid aggregate DISTINCT independently selects and requires its sole
argument's collation for membership. MIN/MAX use the selected invocation
collation at runtime.

Aggregate-call expression properties always have `TypeAffinity::kNone` and no
declared type. The first explicit COLLATE propagated from ordinary arguments
may become the result expression's collation, matching existing scalar-call
property propagation. This result collation remains separate from the
invocation collation.

FILTER and local ordering do not determine aggregate result collation.

### 6. Deduplicate invocations in SQLite occurrence order

Binding records aggregate occurrences without publishing final invocation
IDs. After all clauses are resolved, the binder visits pending occurrences in
this fixed order:

1. result columns from left to right;
2. singleton top-level ORDER BY terms from left to right; and
3. HAVING.

Within one expression, ordinary left-to-right recursive binding order is
retained. Each candidate compares:

- aggregate descriptor identity;
- invocation collation;
- bound ordinary arguments;
- DISTINCT;
- bound local order expressions, collation, direction, and NULL placement;
  and
- optional FILTER.

Structurally equivalent candidates in the same query core share one
`BoundAggregateInvocationId`. Different cores never share invocations.
Every aggregate-call expression is rewritten to its final invocation ID
before `BoundSelect` is returned.

Structural comparison canonicalizes a table's declared `INTEGER PRIMARY KEY`
source column and its hidden rowid names to one physical source identity.
This matches SQLite's resolver representation and allows equivalent aggregate
arguments to share one invocation. An implicit reference to that declared
column also uses the rowid's BINARY effective collation; an explicit COLLATE
wrapper still overrides it.

`BindOptions::maximum_aggregate_terms`, defaulting to 2,000, applies
independently to every query core. Each core checks:

- unique aggregate invocations; and
- unique retained source identities across source and representative leaves.

Either table exceeding the option reports `kAggregateTermLimitExceeded`.
The option participates in the existing 32-bit representability validation.
It is appended after the pre-existing boolean option so positional
four-field `BindOptions` initialization retains its prior meaning.
Statement-global IDs may still back the immutable tables; each
`BoundAggregateProgram` records only its core's IDs.

### 7. Bind GROUP BY aliases, ordinals, collations, and result matches

For each GROUP BY term:

1. classify transparent parentheses, repeated unary plus/minus, and one outer
   COLLATE using ADR-0054's signed-32-bit ordinal rules;
2. report `kGroupByTermOutOfRange` when a recognized ordinal is outside
   `1..result_count`;
3. for an in-range ordinal, bind a reference to that exact result expression,
   preserve an outer COLLATE wrapper, reject the substituted expression when
   it contains an aggregate, and record that exact result-column index;
4. otherwise bind as an ordinary expression with source columns before result
   aliases and aggregates disabled;
5. require its effective collation, defaulting to BINARY; and
6. only for an ordinarily resolved term, record the rightmost structurally
   equivalent result column, if any.

Larger integer spellings are ordinary constant grouping expressions.

The rightmost match is metadata only. A later grouping package may reuse key
storage only for that selected target. A selected result that is a direct
representative source column or rowid remains representative rather than
being replaced by a key reference.

### 8. Transfer eligible HAVING leaves before leaf analysis

Transfer occurs only when GROUP BY is nonempty. The binder flattens top-level
logical AND leaves in source order.

A leaf moves to WHERE when:

- it is not known always false;
- it contains no aggregate call;
- every nonconstant visited subtree is structurally equivalent to at least
  one BINARY-collated GROUP BY term, allowing a difference only in the
  subtree's top-level COLLATE wrapper; and
- it contains no deferred nested-query or outer-aggregate state.

Literal, parameter, and deterministic scalar-only subtrees are constant.
Source columns and rowid are nonconstant unless a matching GROUP BY subtree
prunes the walk.

Moved leaves append to the existing WHERE predicate with logical AND in their
original order. Remaining leaves rebuild HAVING in original order; no
remaining leaf means no HAVING predicate. This immutable result is
SQL-equivalent to SQLite's replacement with integer `1`.

Global aggregates without GROUP BY never transfer HAVING.

### 9. Publish source and representative leaf analysis

After transfer and invocation deduplication, the binder collects unique
source-column and rowid leaf expression IDs.

`source_leaves` come from:

- GROUP BY expressions;
- aggregate ordinary arguments;
- aggregate FILTER predicates; and
- aggregate local ordering, except MIN/MAX local ordering because it is
  resolved but not evaluated.

`representative_leaves` come from result, singleton ORDER BY, and remaining
HAVING expressions outside aggregate-call references.

Traversal follows alias references but does not descend through an aggregate
call when collecting representative leaves. Leaves are deduplicated by bound
source identity while preserving first occurrence. A declared `INTEGER
PRIMARY KEY` source column and the same table's hidden rowid names count as
one retained identity.

This package records dataflow only. It does not allocate representative
values, group records, sorters, aggregate state, registers, or bytecode.

### 10. Keep downstream execution explicitly unsupported

Direct `BindSelectStatement` accepts aggregate queries and publishes the
contracts above.

This package originally stopped logical planning when it detected a
`BoundAggregateProgram`. ADR-0061 supersedes that temporary boundary with
immutable logical and physical aggregate plans. Lowering now returns the
typed unsupported-feature error until aggregate bytecode and execution are
implemented, so read/write session preparation still fails rather than
executing an aggregate query as an ordinary SELECT.

### 11. Verify binding independently

Development follows red-green-refactor. Package-local tests cover:

- every aggregate name/arity, scalar MIN/MAX coexistence, count-star, ALL,
  DISTINCT arity, FILTER, local ordering, and nested aggregate errors;
- result-established and GROUP-established aggregate status;
- HAVING legality and WHERE/GROUP/ORDER aggregate misuse;
- source-versus-alias precedence in GROUP BY and HAVING;
- signed/parenthesized/collated GROUP BY ordinals and out-of-range errors;
- aggregate result affinity, collation, declared type, and alias metadata;
- per-core structural deduplication and result/ORDER/HAVING invocation order;
- compound-arm independence and compound ORDER BY noninterference;
- rightmost GROUP BY result matching;
- HAVING transfer normal, constant, BINARY/non-BINARY, always-false,
  aggregate, and alias cases;
- source and representative leaf sets, including MIN/MAX local-order
  exclusion;
- configured aggregate-term, result-column, function-argument, parameter, and
  allocation limits;
- exact allocation budgets plus exhaustive binder OOM coverage; and
- public session prepare rejection at the planner boundary.

No aggregate SQL execution, grouping, state stepping, DISTINCT membership,
FILTER schedule, local sorting, or result row is accepted by this package.

Delivery update: ADR-0069 consumes the bound DISTINCT, FILTER, local-order,
collation, and derived single-key argument-reuse metadata while preserving
this binder-owned semantic boundary.

## Consequences

### Positive

- Aggregate semantics become an immutable binder contract before planning or
  VM complexity begins.
- Invocation deduplication, representative dataflow, and HAVING transfer are
  tested without temporary storage or bytecode.
- Existing compound ORDER BY behavior remains isolated from arm aggregate
  analysis.
- Downstream packages receive stable IDs and owned metadata rather than syntax
  or registry pointers.

### Negative

- Binding needs a pending aggregate-occurrence table before final IDs can be
  assigned in SQLite order.
- Aggregate queries bind successfully but remain intentionally unplannable for
  one package.
- HAVING transfer adds a focused immutable-expression rewrite inside the
  binder.

## Rejected alternatives

### Bind and intern aggregate calls immediately

Rejected because SQLite invocation order places singleton ORDER BY before
HAVING even though HAVING resolves earlier for error compatibility.

### Reuse scalar-call payloads for aggregate calls

Rejected because aggregate calls reference per-group invocation state and
must not be evaluated as eager scalar callbacks.

### Let planner rediscover aggregate calls and representative leaves

Rejected because that repeats semantic resolution, risks different alias and
collation behavior, and couples planning to syntax details.

### Delay HAVING transfer to optimization

Rejected because the accepted roadmap fixes its matching and observable error
order as aggregate binding behavior.

### Permit aggregate execution through the existing ordinary plan

Rejected because it would silently evaluate bare columns per source row and
produce success-shaped nonaggregate results.

## References

- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0015: SQL Values, Affinity, and Comparison
- ADR-0016: Collation Contracts and Built-ins
- ADR-0017: Scalar Function Registry
- ADR-0033: Immutable Read-Only SELECT Binding
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0059: SQLite-Aligned Aggregate Function State
