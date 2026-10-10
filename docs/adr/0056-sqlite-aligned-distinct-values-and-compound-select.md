# ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT

- Status: Accepted
- Date: 2026-10-09

## Context

ADR-0053 places `DISTINCT`, top-level `VALUES`, and compound SELECT
immediately after ORDER BY. This slice introduces the keyed temporary-relation
storage and lifecycle foundation that later grouping, automatic indexes,
recursive CTEs, and other advanced SQL features must extend and reuse.

The current query pipeline supports:

- one simple SELECT core;
- zero or one table source;
- WHERE, projection, ORDER BY, LIMIT, and OFFSET;
- table, rowid, covering-index, and noncovering-index access;
- a spill-capable external sorter;
- a specialized bounded top-N relation; and
- discard-only ephemeral Pager and encoded index-record mutation.

The parser already recognizes `SELECT DISTINCT`, but the binder rejects it.
The lexer recognizes `VALUES`, `UNION`, `INTERSECT`, and `EXCEPT`, while the
parser intentionally rejects top-level VALUES and every compound operator.
There is no general membership/materialization relation, query-arm model,
set-operation plan, or merge execution contract.

These features cannot be implemented as unrelated containers:

- DISTINCT membership is database-sized and must spill under file-backed
  temporary storage;
- compound equality depends on SQLite storage classes, numeric equivalence,
  NULL equality, and per-column collations without applying affinity;
- UNION, INTERSECT, and EXCEPT retain different representatives when two
  records compare equal;
- VALUES preserves row-major expression order and is not subject to SQLite's
  ordinary compound-term limit when it remains a multi-row VALUES clause;
- compound ORDER BY may reference only output columns, aliases, or matching
  output expressions across any arm;
- ordered UNION ALL propagates a positive LIMIT bound into each arm and may
  skip non-key result expressions for rejected rows;
- LIMIT zero halts before OFFSET or any arm expression executes;
- UNION with a literal LIMIT 1 and no OFFSET has a pinned short-circuit path;
- reset, errors, statement destruction, and VM halt must remove every
  temporary file and capability; and
- no database-sized SQL feature may rely only on an unbounded in-memory
  `std::set`, hash table, or vector.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `parse.y:609-689`, where SELECT cores, left-associated compound operators,
  and multi-row VALUES are parsed;
- `resolve.c:1637-1725`, where compound ORDER BY terms are matched against
  result columns and expressions from left to right across arms;
- `resolve.c:2080-2144`, where arm widths are checked and compound ORDER BY is
  resolved after every arm;
- `select.c:904-996`, where DISTINCT uses NULL-equal record membership or
  adjacent ordered comparison;
- `select.c:1198-1388`, where result expressions execute before DISTINCT and
  surviving rows proceed to ORDER BY;
- `select.c:2624-2681`, where compound comparison collations come from the
  leftmost arm that supplies a collation;
- `select.c:2903-2979`, where multi-row VALUES bypasses the ordinary compound
  recursion limit and integer-one `UNION ... LIMIT` forms are recognized;
- `select.c:2983-3162`, where compound width errors, global LIMIT state, and
  UNION ALL short-circuiting are implemented;
- `select.c:3395-3740`, where ordered arms are materialized and merged;
- `select.c:5590-5672`, where explicit compound ORDER BY COLLATE terms force
  set evaluation before an outer sort;
- `expr.c:2959-2977`, where recursively signed integer limits are classified;
- `insert.c:646-699`, where multi-row VALUES chooses between producer-eager
  coroutine and consumer-filtered UNION ALL schedules;
- `vdbe.c:4686-4750`, where `OP_OpenEphemeral` creates a transient B-tree;
- `vdbe.c:5506-5568`, where encoded keys are tested for membership;
- `vdbe.c:6728-6794`, where ephemeral index records are inserted; and
- `vdbe.c:6800-6848`, where matching ephemeral records are deleted.

Reference probes against that snapshot additionally establish:

- `SELECT 1 UNION SELECT 1.0` returns the right representative `1.0`;
- INTERSECT retains the left representative;
- simple DISTINCT retains the first source representative;
- compound operators associate strictly left to right;
- a later arm may supply the comparison collation for all earlier arms;
- UNION ALL without ORDER BY stops arm evaluation when the global LIMIT is
  reached;
- recognized integer-one `UNION ... LIMIT` forms may stop after the first row
  of the left arm;
- ordered UNION ALL applies bounded ordering independently to each arm, so
  non-key payload evaluation is arm-local; and
- empty-left EXCEPT and INTERSECT skip the right arm, while other set forms
  with ordinary LIMIT values evaluate every required arm before output.

## Decision

### 1. Scope this slice to one query block with simple arms

Accept:

```sql
select-core
[compound-operator select-core] ...
[ORDER BY ordering-term [, ...]]
[LIMIT expression [OFFSET expression]
 | LIMIT offset-expression , limit-expression]
```

where:

```sql
select-core :=
    SELECT [ALL | DISTINCT] result-column [, ...]
    [FROM one-table [AS alias]]
    [WHERE expression]
  | VALUES (expression [, ...]) [, (expression [, ...])] ...

compound-operator :=
    UNION
  | UNION ALL
  | INTERSECT
  | EXCEPT
```

The operators associate left to right. INTERSECT does not receive higher
precedence.

Each SELECT arm remains limited to the already supported simple source and
WHERE shape. Parenthesized query terms, subqueries, WITH, GROUP BY, HAVING,
windows, joins, table-valued functions, and SELECT as an INSERT source remain
unsupported until their ordered ADR-0053 slices.

ORDER BY and LIMIT/OFFSET are syntactically accepted only after the rightmost
SELECT core. A rightmost VALUES core cannot carry them, matching SQLite:

```sql
VALUES(1) ORDER BY 1;                 -- syntax error
SELECT 1 UNION VALUES(2) LIMIT 1;     -- syntax error
```

An ORDER BY or LIMIT attached before a later compound operator reports the
SQLite-aligned "clause should come after ... not before" error.

Add `ParseOptions::maximum_compound_terms`, defaulting to 500. It limits
explicit SELECT/VALUES cores connected by compound operators. Rows inside one
multi-row VALUES core are stored iteratively and do not consume additional
compound-term budget.

### 2. Model query cores and compounds explicitly in the immutable AST

Replace the single flat SELECT shape with:

```cpp
enum class CompoundOperator : std::uint8_t {
  kUnion,
  kUnionAll,
  kIntersect,
  kExcept,
};

struct SelectCore {
  SourceSpan span;
  SelectQuantifier quantifier = SelectQuantifier::kDefault;
  std::vector<ResultColumn> result_columns;
  std::optional<TableSource> from{};
  std::optional<ExpressionId> where{};
};

struct ValuesCore {
  SourceSpan span;
  std::vector<std::vector<ExpressionId>> rows;
};

using QueryCore = std::variant<SelectCore, ValuesCore>;

struct CompoundTerm {
  SourceSpan span;
  CompoundOperator operation = CompoundOperator::kUnion;
  QueryCore core;
};

struct SelectStatement {
  SourceSpan span;
  QueryCore first;
  std::vector<CompoundTerm> compounds;
  std::vector<OrderingTerm> order_by{};
  std::optional<LimitClause> limit{};
};
```

The parser:

- preserves every core and operator span;
- stores VALUES rows directly rather than encoding them as recursive UNION ALL
  nodes;
- applies `maximum_columns` to every SELECT result list, VALUES row, and ORDER
  BY list;
- permits different VALUES row widths syntactically so binding can report the
  SQLite-compatible semantic error;
- retains statement-local expression IDs and prepare-tail behavior; and
- rejects a trailing operator, empty VALUES row, malformed row separator, or
  misplaced clause without returning a partial tree.

### 3. Bind all arms into one immutable query object

`BoundSelect` becomes a query-block object with shared:

- source text and catalog snapshot;
- scalar-function and collation registrations;
- parameter numbering and binding metadata;
- one global expression arena; and
- one global source-column arena partitioned into per-core ranges.

Add immutable core records:

```cpp
struct BoundSelectCore {
  SelectQuantifier quantifier = SelectQuantifier::kDefault;
  std::optional<BoundTableSource> table_source{};
  BoundIdRange<BoundSourceColumnId> source_columns{};
  std::vector<BoundResultColumn> result_columns;
  std::optional<BoundExpressionId> where{};
};

struct BoundValuesCore {
  std::vector<std::vector<BoundExpressionId>> rows;
  std::size_t column_count = 0;
};

using BoundQueryCore = std::variant<BoundSelectCore, BoundValuesCore>;

struct BoundCompoundTerm {
  CompoundOperator operation = CompoundOperator::kUnion;
  BoundQueryCoreId core;
};
```

Parameters are statement-global across all arms and VALUES rows. A source name
is visible only inside its own SELECT core. VALUES expressions bind with an
empty source scope.

All arms must have the same output width. VALUES rows must have the same
width. Report:

- `all VALUES must have the same number of terms`; or
- `SELECTs to the left and right of <operator> do not have the same number of
  result columns`.

Result names come from the leftmost core. Declared-type and affinity metadata
normally come from that core as well. When an explicit compound ORDER BY
COLLATE term selects the set-then-order rewrite, pinned SQLite's outer ordered
projection keeps the leftmost names but publishes declared types and affinities
from the rightmost core. A VALUES core publishes `column1`, `column2`, and so
on.

No affinity is applied while comparing compound or DISTINCT keys. Runtime
storage classes remain those produced by each arm.

### 4. Pin DISTINCT and compound comparison semantics

DISTINCT compares every projected result column using:

- SQLite numeric equivalence, so INTEGER 1 and REAL 1.0 are equal;
- NULL-equal semantics, so all NULL values in one column compare equal;
- storage-class ordering for otherwise different types;
- the expression's resolved collation when present; and
- BINARY when no expression collation is present.

Compound equality and iteration use one statement-wide comparison descriptor.
For each output column, choose the first arm from left to right whose
corresponding result expression supplies a collation. If no arm supplies one,
use BINARY. Explicit `COLLATE BINARY` counts as supplying a collation and
therefore blocks later-arm collations.

This descriptor applies to every set node in a mixed compound, including
comparisons among earlier arms when a later arm is the first to supply the
collation.

Equivalent encoded records retain representatives as follows:

| Construct | Retained record |
|---|---|
| SELECT DISTINCT | first source occurrence |
| UNION | right operand representative |
| INTERSECT | left operand representative |
| EXCEPT | no record when the right operand matches |

For example, `SELECT 1 UNION SELECT 1.0` emits REAL 1.0, while
`SELECT 1 INTERSECT SELECT 1.0` emits INTEGER 1.

### 5. Add one general spill-capable keyed ephemeral relation

Add to `temporary_storage`:

```cpp
struct EphemeralRelationDescriptor {
  std::size_t field_count = 0;
  std::size_t key_field_count = 0;
  std::vector<IndexColumnOrder> key_columns;
  RecordCodecOptions record_options{};
};

enum class EphemeralInsertMode : std::uint8_t {
  kKeepExisting,
  kReplaceExisting,
};

enum class EphemeralInsertResult : std::uint8_t {
  kInserted,
  kDuplicate,
  kReplaced,
};
```

The move-only relation supports:

```text
open/writing -> rewound/positioned -> exhausted
     |                |
     +---- reset -----+
     +---------------> closed
```

Operations are:

- insert an owned encoded record with keep-existing or replace-existing
  semantics;
- test an encoded key prefix for membership;
- erase an encoded key prefix;
- rewind to the smallest key;
- read the borrowed current record;
- advance;
- reset/clear; and
- close.

Writes after rewind and reads before positioning are misuse errors. Duplicate
membership is an ordinary result, not a constraint error.

File mode uses:

- `Pager::OpenEphemeral()`;
- one encoded index B-tree rooted at page two;
- the descriptor's unique key prefix and optional payload suffix;
- shared record comparison, collation, overflow, rebalance, and cleanup
  contracts; and
- checked replacement as delete-equivalent-key followed by insert-new-record.

Memory mode uses an owning ordered standard container over encoded records and
the same comparator. It catches allocation and length failures and never
creates a file. This is permitted because the default file backend remains the
database-sized implementation; an unbounded memory container is not the only
implementation.

Any checked encode, compare, allocation, Pager, B-tree, cursor, or VFS failure
closes the relation and removes its transient file. Reset clears the existing
root/backend and does not silently replace a failed relation with a new one.

The relation is internal temporary state, not a persistent database format.
It performs no main-database transaction, journal, schema, or catalog work.

### 6. Reuse encoded index-prefix primitives instead of duplicating a tree

Extend `IndexBtreeWriter` only with the narrow encoded operations required by
the relation:

- insert-if-absent with a typed inserted/duplicate result;
- replace an equivalent encoded key while preserving the new complete record;
- encoded prefix membership; and
- existing encoded delete/clear behavior.

All operations reuse the shared prefix seek and `CompareRecordPrefixes()`.
They do not expose VM, planner, or temporary-relation types to the storage
layer.

Memory and file relation backends must pass the same normal, duplicate,
replace, delete, collation, storage-class, overflow, reset, OOM, and injected
I/O vectors.

### 7. Extend logical and physical plans by semantic phase

Logical planning adds:

```cpp
struct LogicalValuesNode;
struct LogicalDistinctNode;
struct LogicalCompoundNode;
```

Each simple SELECT core retains source, WHERE, and projection phases.
DISTINCT is after projection. Compound nodes combine complete child row
streams. Statement ORDER BY and LIMIT/OFFSET remain above the compound root.

The logical tree preserves left association for mixed operators.

Physical planning chooses:

- `kEphemeralMembership` for simple DISTINCT;
- `kConcatenate` for unordered UNION ALL;
- `kUnionLimitOne` for a recognized integer-one UNION limit without ORDER BY
  or OFFSET;
- `kEphemeralSet` for unordered UNION, INTERSECT, and EXCEPT;
- `kOrderedMerge` for merge-compatible compounds with ORDER BY;
- `kSetThenOrder` when a compound containing UNION, INTERSECT, or EXCEPT has
  any explicit ORDER BY COLLATE term; and
- the existing external sorter or runtime top-N strategy inside ordered arms.

No hash-only DISTINCT or set implementation is added. Adjacent-distinct,
unique-access, index-order, and other eliminations require later measured
evidence.

### 8. Execute simple DISTINCT before ORDER BY

For each source row:

1. evaluate every projected result expression exactly once;
2. encode the complete result row;
3. insert it into the DISTINCT relation with `kKeepExisting`;
4. skip duplicates before evaluating hidden ORDER BY expressions;
5. without ORDER BY, apply OFFSET/LIMIT and emit the surviving first
   representative immediately; or
6. with ORDER BY, evaluate ordering expressions for the surviving row and
   insert it into the existing sorter or top-N relation.

Consequences:

- DISTINCT without ORDER BY preserves first-occurrence scan order;
- a positive LIMIT may stop source evaluation after enough distinct rows;
- DISTINCT with ORDER BY processes the complete source unless LIMIT is zero;
- hidden ORDER BY expressions do not execute for duplicate rows;
- result expressions cannot use the ORDER BY top-N payload deferral because
  they are required for membership first; and
- LIMIT zero halts before OFFSET, relation open, source access, or expression
  evaluation.

### 9. Execute VALUES in row-major order

A standalone VALUES core evaluates:

- rows from left to right; and
- expressions within each row from left to right.

It emits duplicates unchanged and performs no implicit sorting or membership
check.

Binding publishes one of two VALUES schedules:

```cpp
enum class ValuesEvaluationSchedule : std::uint8_t {
  kProducerEager,
  kConsumerFiltered,
};
```

Use `kProducerEager` when:

- every row expression is constant under SQLite's VALUES classifier:
  literals, parameters, operators over constant operands, and deterministic
  scalar functions over constant operands;
- no row expression contains `IS` or `IS NOT`; and
- every first-row expression has `TypeAffinity::kNone`.

Otherwise use `kConsumerFiltered`. CAST or any other first-row expression
with affinity selects `kConsumerFiltered`.

When a VALUES core is streamed through unordered UNION ALL:

- `kProducerEager` evaluates a row before consumer OFFSET decides whether to
  discard it; and
- `kConsumerFiltered` applies OFFSET/LIMIT before evaluating a skipped row.

This intentionally preserves pinned failure timing. For example, OFFSET skips
`CAST(failing() AS TEXT)` in the first VALUES row but does not skip the same
deterministic failing call without CAST.

LIMIT zero still halts before either schedule evaluates a row. When VALUES is
an arm of a set operator or ordered compound, it follows that operator's
materialization rules and all required rows execute regardless of the
streaming schedule.

VALUES row storage is iterative; thousands of rows do not create parser,
planner, lowering, or VM recursion proportional to row count.

### 10. Materialize unordered set operators with the keyed relation

Unordered compounds retain SQLite's observed output order by iterating the
full-row relation in ascending compound comparison order.

Fold mixed operators from left to right:

- **UNION ALL**: stream the left child followed by the right child with one
  shared LIMIT/OFFSET state.
- **UNION**:
  1. materialize the left child into a keep-existing relation;
  2. materialize the right child separately with keep-existing semantics so
     duplicates within the right child retain its first representative;
  3. iterate the right relation and replace equivalent records in the left
     relation; and
  4. iterate the resulting relation.
- **EXCEPT**:
  1. materialize the left child with keep-existing semantics;
  2. if the left relation is empty, finish without opening or evaluating the
     right child;
  3. otherwise evaluate the right child completely and erase each matching
     left key; and
  4. iterate the remaining left relation.
- **INTERSECT**:
  1. materialize the left child with keep-existing semantics;
  2. if the left relation is empty, finish without opening or evaluating the
     right child;
  3. otherwise materialize the right child with keep-existing semantics;
  4. iterate the left relation;
  5. insert contained left records into a result relation; and
  6. iterate the result.

If a set operator consumes a UNION ALL child, materializing that child with
keep-existing semantics removes its duplicates while preserving the first
representative in the child's output order.

Fold iteratively and close consumed child relations immediately. No unordered
set step keeps more than left, right, and output relations live
simultaneously.

Except for the integer-one UNION path and empty-left EXCEPT/INTERSECT,
unordered set operators evaluate both required children completely before
returning the first row.

### 11. Preserve the recognized UNION LIMIT 1 short circuit

For:

```sql
left UNION right LIMIT 1
```

with:

- an integer literal whose value becomes exactly 1 after stripping transparent
  parentheses and applying any chain of unary `+` and `-`;
- no OFFSET; and
- no ORDER BY,

use the dedicated `kUnionLimitOne` strategy:

1. request at most one row from the left subtree;
2. return it immediately if present without evaluating the right subtree;
3. otherwise request at most one row from the right subtree.

The first left row is a valid UNION result even if later rows would compare
smaller. This strategy intentionally differs from ordinary unordered UNION,
which materializes and returns full-row comparison order.

Recognize `1`, `+1`, `(1)`, and `-(-1)`. Do not recognize parameters, real
literals, casts, or arithmetic such as `1+0`.

Unrecognized expressions that evaluate to 1 use the ordinary set strategy.
The classifier is observable because it controls whether the right arm
executes; false positives and false negatives are therefore both forbidden.

### 12. Use per-arm ordered streams and a stable merge

Compound ORDER BY terms may resolve only to:

- a 1-based result ordinal;
- an output alias from any arm, searched left to right; or
- an expression structurally identical to an output expression from any arm,
  searched left to right.

An unmatched expression reports:

`N-th ORDER BY term does not match any column in the result set`.

The effective ORDER BY collation is:

1. explicit COLLATE on the ordering term;
2. the compound output column's comparison collation; or
3. BINARY.

Do not feed all arms into one global top-N relation. Pinned SQLite applies
bounded ordering independently to UNION ALL arms, which changes non-key
payload evaluation.

For merge-compatible ORDER BY, use:

1. publish one arm-specific output-field mapping for the compound ORDER BY;
2. materialize each arm through the existing external sorter;
3. for UNION ALL with a safe positive runtime `LIMIT + OFFSET` bound, use one
   top-N relation per arm and preserve key-before-admission payload deferral;
4. for UNION with the exact integer-one limit classifier and no OFFSET, use a
   bound-one top-N relation per arm, with every result column represented in
   the complete merge key;
5. for other UNION, INTERSECT, and EXCEPT forms, evaluate complete result rows
   and use external arm sorters because full-row equality is required;
6. rewind the left stream before opening/building the right stream for EXCEPT
   or INTERSECT, and finish immediately if the left stream is empty;
7. otherwise rewind child streams and merge them by the ORDER BY key;
8. for non-ALL operators, append every result column not already covered by
   ORDER BY to the merge key in ascending compound-comparison order so equal
   complete rows are adjacent;
9. preserve left-arm precedence for equal UNION ALL sort keys;
10. apply the operator-specific representative rules at equal complete rows;
    and
11. apply global OFFSET/LIMIT while draining the merged stream.

An explicit COLLATE on any compound ORDER BY term is not merge-compatible
when the compound contains UNION, INTERSECT, or EXCEPT. The explicit ordering
collation may differ from the compound duplicate-comparison collation, so
equivalent set rows are not guaranteed to be adjacent in ORDER BY order.

For that shape, `kSetThenOrder`:

1. evaluates the complete compound without ORDER BY using the keyed set
   contracts above;
2. preserves the operator-specific representatives under the compound
   comparison descriptor; and
3. feeds those final rows into one outer sorter/top-N relation using the
   explicit ORDER BY descriptor.

This fallback is not required for a compound containing only UNION ALL.
All child result expressions execute before the outer sorter, so it does not
apply per-arm non-key payload deferral.

For example:

```sql
SELECT 'A' COLLATE NOCASE
UNION SELECT 'B'
UNION SELECT 'a'
ORDER BY 1 COLLATE BINARY;
```

first collapses `'A'`/`'a'` under NOCASE, retaining the right representative
`'a'`, then orders `'B'`, `'a'` under BINARY.

Arm evaluation remains left to right. The implementation must not keep one
live sorter per possible compound term.

For a maximal associative run of three or more arms, equivalently two or more
consecutive UNION ALL or UNION operators:

1. keep one external group sorter open;
2. materialize each arm through its required local sorter or top-N in source
   order;
3. for UNION, deduplicate adjacent complete keys within each arm first,
   retaining that arm's first representative;
4. drain and close that arm before opening the next arm;
5. rely on stable group-sorter insertion order for equal UNION ALL keys; and
6. for UNION, deduplicate adjacent complete keys once after the group sort,
   retaining the representative from the last contributing arm.

This grouped materialization avoids both one live sorter per arm and the
quadratic rewriting of an ever-growing left accumulator. Exactly two arms and
every EXCEPT, INTERSECT, or mixed barrier use the direct two-stream merge. At
most three ordering or membership capabilities and their transient files are
live at once. EXCEPT, INTERSECT, and mixed operators retain left association.

No general coroutine bytecode is introduced in this slice. Ordered arm
materialization provides the required observable behavior without adding the
subquery/CTE coroutine lifecycle early.

### 13. Add typed relation and merge bytecode

Add immutable relation descriptors and instructions:

```text
open relation
insert relation (keep or replace, duplicate target)
contains relation (found target)
delete relation
rewind relation (empty target)
read relation field
next relation
reset relation
close relation
```

Add one record-order comparison instruction that compares two consecutive
register ranges through an immutable comparison descriptor and returns
less/equal/greater. It reuses the canonical SQL value, collation, direction,
and NULL-placement comparator used by sorters and indexes.

The verifier tracks relation states exactly:

- closed;
- writing;
- positioned;
- exhausted.

It rejects:

- use before open;
- insert/delete after rewind;
- read/next before positioning;
- field/count mismatches;
- invalid duplicate/found branches;
- incompatible relation states at control-flow joins; and
- reachable program exit with an unclosed capability unless normal VM cleanup
  owns that exit.

VM reset, halt, checked error, finalize, and destruction close every relation
and arm sorter/top-N capability.

### 14. Preserve LIMIT/OFFSET evaluation and row completion

Evaluate the statement LIMIT expression once before any core. Convert it
strictly to signed 64-bit as the existing SELECT path does.

- LIMIT zero halts before OFFSET, arm expressions, source cursors, sorters, or
  relations open.
- OFFSET executes only for nonzero LIMIT.
- Negative OFFSET normalizes to zero.
- Negative LIMIT means unlimited.
- Overflow in positive `LIMIT + OFFSET` selects external sorting rather than a
  bounded relation.

UNION ALL without ORDER BY decrements shared OFFSET/LIMIT while streaming arms
and does not open a relation.

Set operators and DISTINCT count rows only after duplicate suppression.
Compound ORDER BY counts rows only while draining the merged stream.

A read statement may return earlier UNION ALL or DISTINCT rows and later
return a checked scalar, allocation, I/O, or encoding error. Materializing set
operators finish all required child expression evaluation and relation
construction before their first row. Their final file-backed drain may still
return a checked read, payload-copy, decode, or cursor-advance failure after
earlier result rows.

### 15. Test each behavior before implementation

Deliver this slice through ordered red-green-refactor PRs:

1. query-core, VALUES, and compound syntax;
2. immutable binding and logical/physical plans;
3. general keyed ephemeral relation;
4. relation bytecode, verifier, and VM lifecycle;
5. simple DISTINCT execution;
6. VALUES and unordered UNION ALL;
7. unordered UNION, EXCEPT, and INTERSECT;
8. compound ORDER BY and stable merge;
9. public-session, model, differential, fuzz, and allocation completion; and
10. standalone performance baseline.

Syntax and binding tests cover:

- every operator and mixed left association;
- misplaced ORDER BY/LIMIT;
- trailing operators and malformed VALUES rows;
- compound and column resource limits;
- VALUES width and compound width errors;
- leftmost names and `columnN` names;
- parameter sharing across arms;
- compound ORDER BY aliases, ordinals, exact expressions, and failures;
- global collation selection including late-arm and explicit BINARY cases;
- integer/real, text, blob, and NULL equality; and
- unsupported parenthesized/subquery boundaries.

Temporary-relation tests cover:

- memory/file parity;
- keep, duplicate, replace, contains, erase, iteration, reset, and close;
- right-representative replacement of comparator-equivalent encodings;
- NULL, numeric, storage-class, BINARY, NOCASE, and RTRIM keys;
- duplicate key prefixes with payload suffixes;
- overflow keys and payloads;
- B-tree split, rebalance, clear, and cache pressure;
- deterministic file removal;
- injected open/read/write/encode/compare failures;
- exhaustive OOM and allocation matrices; and
- invalid lifecycle calls.

Execution tests cover:

- DISTINCT first-representative behavior;
- DISTINCT hidden ORDER expressions skipped for duplicates;
- DISTINCT LIMIT short-circuiting;
- VALUES row-major evaluation;
- producer-eager versus consumer-filtered VALUES OFFSET call/error schedules;
- UNION right representatives;
- INTERSECT left representatives;
- EXCEPT removal;
- empty-left EXCEPT and INTERSECT skipping right-arm expressions and failures;
- no-affinity compound equality;
- late-arm collations;
- mixed operators;
- UNION ALL streaming LIMIT/OFFSET;
- recognized `1`, `+1`, `(1)`, and `-(-1)` UNION limits skipping the right arm;
- parameters, reals, casts, and `1+0` taking the ordinary UNION path;
- ordered UNION ALL per-arm lazy payload calls;
- ordered UNION LIMIT 1 per-arm bounded storage;
- ordered set-operator full evaluation;
- explicit ORDER BY COLLATE set-then-order behavior where duplicate and order
  collations differ;
- equal-key arm stability;
- bounded live-capability/file counts for long compounds;
- reset, rebinding, statement destruction, construction-time failures, and
  later-page drain failures after prior rows; and
- exact verifier state joins.

### 16. Extend compatibility, model, fuzz, and performance evidence

Add pinned SQLite differential cases for:

- simple and ordered DISTINCT;
- VALUES widths, names, row order, and malformed forms;
- UNION, UNION ALL, EXCEPT, and INTERSECT;
- mixed left-associated operators;
- representative storage classes;
- NULL and numeric equality;
- BINARY, NOCASE, RTRIM, explicit BINARY, and late-arm collations;
- compound ORDER BY resolution;
- explicit-COLLATE set-then-order rewrites;
- empty-left EXCEPT/INTERSECT right-arm suppression;
- integer-one UNION classifier boundaries;
- VALUES OFFSET evaluation schedules;
- positive, zero, negative, comma, parameterized, and overflowing limits;
- function call logs and failure timing; and
- unsupported query-term boundaries.

The model tier generates bounded VALUES and table-backed arms and compares
row sequences, representatives, and errors against a deterministic reference
model.

Fuzzing adds query-core/operator/VALUES generation while preserving limits on
source bytes, columns, expressions, and compound terms.

Create a separate version-1 DISTINCT/compound performance contract. Existing
read, index, write, and ORDER BY baseline input identities remain unchanged.
Record at least:

- low- and high-cardinality DISTINCT;
- collated DISTINCT;
- DISTINCT ORDER BY LIMIT;
- large VALUES and UNION ALL LIMIT;
- file-backed UNION with comparator-equivalent replacements;
- EXCEPT and INTERSECT membership;
- multi-arm ordered UNION ALL top-N;
- ordered UNION/INTERSECT/EXCEPT merge;
- long mixed compounds; and
- a pre-optimization keyed-relation baseline.

Pin fixture, SQL, page/cache/temp-store settings, relation backend, sorter
threshold, result digest, source-row work, spill evidence, and call counts.
Every repetition must reach the timing floor, and aggregate plus every paired
round must remain within the accepted 10x wall and CPU guard.

## Explicit deferrals

This slice does not implement:

- GROUP BY, HAVING, aggregate functions, or aggregate-local DISTINCT;
- joins or multi-source namespaces;
- scalar, EXISTS, IN, or FROM-clause subqueries;
- parenthesized query terms;
- WITH or recursive CTEs;
- INSERT SELECT or CREATE TABLE AS SELECT;
- adjacent-distinct, unique-access, or index-order DISTINCT elimination;
- hash-only DISTINCT or set operators;
- general coroutine/subroutine bytecode;
- recursive queue relations; or
- automatic indexes, including non-unique shorter-prefix range iteration over
  the keyed-relation storage foundation.

Those remain in the ADR-0053 order.

## Consequences

### Positive

- One spill-capable keyed-relation foundation serves DISTINCT and set
  operators and is explicitly extensible by later advanced SQL consumers.
- Duplicate equality and representative retention are explicit and testable.
- VALUES no longer abuses recursive compound nodes.
- UNION ALL preserves streaming and LIMIT short-circuit behavior.
- Ordered compounds reuse the proven sorter/top-N implementation while
  matching arm-local payload evaluation.
- Temporary storage remains below the VM and outside main-database
  transactions.
- Query planning advances from one linear SELECT to an immutable query-block
  tree without exposing syntax types to the VM.

### Negative

- The binder, logical plan, physical plan, and lowering interfaces require a
  substantial one-time query-core refactor.
- File and memory relation backends add a new lifecycle and failure matrix.
- Ordered compound merge adds comparison and capability state to bytecode.
- Unordered set materialization may be slower than SQLite's current merge
  implementation until profiling justifies a measured optimization.
- Long compounds require careful bounded-resource merge planning.

### Risks

- Retaining the wrong comparator-equivalent record changes `typeof()` and
  output bytes even when equality is correct.
- Applying affinity to set keys incorrectly merges text, numeric, or blob
  values.
- Choosing collations per node instead of per complete compound changes
  earlier-arm equality.
- One global top-N for ordered UNION ALL changes function evaluation.
- Treating INTERSECT as higher precedence changes mixed compounds.
- Relation failure cleanup can leak transient files if it diverges from
  Pager/top-N lifecycle rules.

These risks are addressed by pinned representative, call-log, collation,
spill, OOM, I/O, verifier, and differential matrices.

## Rejected alternatives

### Use `std::unordered_set` for DISTINCT and compounds

Rejected because cardinality is database-controlled, collation and numeric
equality are not ordinary C++ hashing, iteration order would not match pinned
SQLite, and there would be no spill or VFS failure surface.

### Use only the external sorter for every set operation

Rejected because DISTINCT without ORDER BY must stream first representatives
and may stop on LIMIT, while unordered UNION replacement, EXCEPT deletion, and
INTERSECT membership are direct keyed-relation operations reused by later
features.

### Use one global top-N for ordered compounds

Rejected because pinned SQLite applies bounded ordering per UNION ALL arm.
The difference is observable through non-key scalar function calls and
failures.

### Retain a balanced binary tree of materialized arm runs

Rejected for the initial bytecode contract because building a sibling subtree
while retaining the first subtree requires four live capabilities: the
retained run, two sibling inputs, and their output. Avoiding the fourth
capability would require detachable sorter runs, resumable merge coroutines,
or a new temporary-file ownership contract. Maximal associative runs instead
use the stable external group sorter defined above, which preserves left-to-
right evaluation, keeps the three-capability bound, and avoids quadratic
left-deep rewriting without introducing those deferred mechanisms.

### Add general coroutines before subqueries and CTEs

Rejected because fully materialized arm sorters provide the required compound
behavior with existing ownership primitives. General coroutine lifecycles
remain deferred until a later consumer requires resumable query execution.

### Treat INTERSECT as higher precedence

Rejected because SQLite groups compound operators strictly from left to right.

### Keep the flat single-SELECT AST and encode compounds as hidden recursion

Rejected because VALUES row count would consume parser recursion, arm-local
scope would remain implicit, mixed operators would be difficult to validate,
and later query-block features would inherit an unstable representation.

## References

- [ADR-0015: SQL Values, Affinity, and Comparison](0015-sql-values-affinity-and-comparison.md)
- [ADR-0016: Collation Contracts and Built-ins](0016-collation-contracts-and-builtins.md)
- [ADR-0018: SQLite Record Codec](0018-record-codec.md)
- [ADR-0031: Typed Immutable Bytecode Programs](0031-typed-bytecode-programs.md)
- [ADR-0032: Read-Only Bytecode Virtual Machine](0032-read-only-bytecode-virtual-machine.md)
- [ADR-0033: Immutable Read-Only SELECT Binding](0033-immutable-select-binding.md)
- [ADR-0034: Immutable Logical SELECT Plans](0034-immutable-logical-select-plans.md)
- [ADR-0035: Deterministic Basic Read Optimization](0035-deterministic-basic-read-optimization.md)
- [ADR-0036: Physical Read Plan Lowering](0036-physical-read-plan-lowering.md)
- [ADR-0043: SQLite-Compatible B-Tree Mutation](0043-sqlite-compatible-btree-mutation.md)
- [ADR-0044: Reference-Faithful SQLite B-Tree Mutation](0044-reference-faithful-btree-mutation.md)
- [ADR-0045: Existing-Module SQLite Reference Alignment](0045-existing-module-reference-alignment.md)
- [ADR-0046: Canonical Engineering and Reviewable Delivery](0046-canonical-engineering-and-reviewable-delivery.md)
- [ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics](0052-sqlite-aligned-index-planning-maintenance-and-statistics.md)
- [ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery](0053-sqlite-aligned-advanced-sql-architecture.md)
- [ADR-0054: SQLite-Aligned ORDER BY and External Sorter](0054-sqlite-aligned-order-by-and-external-sorter.md)
- [ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N](0055-sqlite-aligned-ephemeral-pager-and-spillable-top-n.md)
