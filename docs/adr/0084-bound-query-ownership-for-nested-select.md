# ADR-0084: Bound Query Ownership for Nested SELECT

- Status: Accepted
- Date: 2026-10-14

## Context

PRs #193 and #194 provide verified calls, private read capabilities, and
closed-return ownership. Scalar and EXISTS consumers now need immutable query
ownership and reentrant binding rather than another evaluator or nested
session.

The current bound statement already owns one source text, catalog snapshot,
parameter map, expression arena, source-column arena, function/collation pools,
and query-core arena. Query-level compound operators, collation, output
metadata, ORDER BY, and LIMIT are still anonymous root-only fields. A nested
query cannot safely overwrite those fields or duplicate the statement arenas.

Independent pinned probes establish that:

- scalar queries publish the first column of the first admitted row, or NULL;
- user LIMIT/OFFSET still apply before that single-value destination;
- scalar declared types and affinity follow the selected expression, while
  scalar subqueries do not implicitly inherit its column collation;
- EXISTS binds its result expressions but skips ordinary result evaluation;
- aggregate work and delayed aggregate errors still occur under EXISTS;
- outer result aliases are not visible inside a nested SELECT; and
- aggregate ownership across outer scopes needs explicit native-aligned
  resolution, not an assumption that every nested invocation is local.

## Decision

### 1. Establish one canonical query owner without changing existing SQL

Add a strong statement-local bound query identity, distinct from source,
expression, and core positions. Root is query zero.

A bound query owns its core range, compound operators, comparison collations,
published output metadata, ordering, and limit. All cores remain in the
existing statement-owned core arena; all expressions, source columns,
parameters, functions, and collations remain statement-global.

Move root query metadata into that same query representation. Existing root
accessors are views of the canonical owner, not copied compatibility data.
Keep per-core binding scratch separate from published query metadata.

Retain the root query inline in the statement implementation. This is the
ordinary no-nesting path and must not gain a preparation allocation merely
to store a one-element query vector. Nested query metadata later uses the same
query type in an owned arena; this inline-root storage exception is limited
to preserving the current root-only allocation contract. It does not create
another binder, comparator, value representation, or query implementation.

This first behavior-preserving ownership checkpoint begins from green tests,
does not need an artificial failing test, and broadens no parser production.

### 2. Keep nested syntax and binding arena based

Nested SELECT syntax refers to statement-owned query records through strong
IDs. No expression owns another syntax tree, bound statement, plan, session,
or recursive smart-pointer graph. Destruction remains nonrecursive.

Binding uses explicit query/core scope state and a shared parameter/expression
context. Current-core sources win, missing local names may use admitted outer
source scopes, and parent result aliases never become a name-resolution
fallback. Correlation captures SQL values through the verified register
interface; a callee cannot borrow a caller cursor.

Every query owns its aggregate legality, output width, clause metadata, and
compiler destination. Outer-owned aggregate resolution is admitted only with
its own native evidence. Query depth, source/column limits, malformed references,
and error phases remain explicit.

Scalar metadata follows the pinned expression rule: the first expression of
the rightmost compound core (or rightmost VALUES row), not the client output
metadata selected for a top-level compound. One declared-type walker handles
plain expressions, scalar query expressions, and VALUES without borrowing
child collation. See pinned `expr.c:45-74` and `select.c:2093-2107`.

Borrowed query views select one canonical metadata owner and core range while
retaining the same statement-global arenas. The binder reserves a query's
complete core range before binding expressions, so a child cannot interleave
its cores with its parent. Reentrant scratch is moved into lexical scope
frames, not copied into independent bound statements. The first executable
admission is uncorrelated SELECT-expression queries; correlation and mutation
destinations remain explicit unsupported-feature boundaries.

Resolution keeps one cumulative expression-height budget across query scopes,
in addition to the parser's shared recursion budget. Each expression root
contributes its syntax height; normalized ON/USING/NATURAL plus WHERE and the
LIMIT/OFFSET wrapper keep their native combined heights. Nested binding must
not reset this budget. Pinned `resolve.c:2211-2220,2257-2265` admits 43 nested
single-value SELECT expressions and rejects 44 at the default height limit;
the read-call ABI's separately tested 64-frame capacity does not broaden that
SQL limit.

### 3. Reuse one compiler and the accepted read-call ABI

Plans identify their query owner and use the existing physical consumers.
Lowering allocates statement-global registers, symbols, cursors, and temporary
descriptors, then emits verified read subroutines for admitted scalar/EXISTS
queries.

A scalar destination initializes NULL, accepts at most one result value after
the query's semantic phases, closes its private resources, and returns.
An EXISTS destination initializes zero, accepts the first produced row,
publishes one without ordinary projection, closes, and returns. Aggregate
evaluation and finalization remain canonical.

Uncorrelated evaluation is lazy and cached for the owning query execution
lifetime. Correlated evaluation receives the current captured values.
Reset/rebind, zero/early LIMIT, suspension, failures, and cleanup keep the
current statement lifecycle. No runtime parsing, preparation, plan
interpretation, nested session, private sorter, or alternate materialization
store is introduced.

Logical and physical query records are flat statement-owned arenas, with
borrowed per-query views and the same builders for every owner. Lowering uses
the same compiler state type per query, one shared bytecode builder, one
expression-register arena, and disjoint query-local scratch/resources. Child
bodies follow main's terminal instruction and use typed private read regions.
The existing row-publication point selects either the client or the scalar/
EXISTS destination; it is not a second producer or execution engine.

Query views borrow their owner until it is moved or destroyed. Expression,
parameter, source, and source-column IDs stay statement global; logical and
physical node IDs are local to the selected query view. No copied root
compatibility representation is retained.

Both expression destinations use the pinned single-row demand rewrite:
without a user limit, add one; retain literal zero/one; otherwise compare the
user limit to numeric-affinity zero. Preserve OFFSET and its integer check.
This controls the canonical bounded ordering and compound consumers as well
as final publication, rather than sorting/evaluating an unrestricted child
and discarding rows afterward. Scalar result affinity and declared type remain
independent of that compiler demand.

EXISTS validates discarded ordering in the same binding arena checkpoint
used by speculative compound-order matching, then restores its generated
expressions, nested queries, resources, and aggregate work. Parameters remain
statement global. Discarded ORDER BY aggregates do not make an otherwise
ordinary query aggregate. DISTINCT is ignored for the EXISTS row destination;
actual grouping, HAVING, and aggregate evaluation retain their errors. A
demand-one UNION does not require a comparison collation it never uses.

Discarded value contexts still resolve names, functions, parameters, and
aggregate legality. They defer scalar-width and executable collation checks
that pinned SQLite performs only while generating the discarded value.
Aggregate arguments and filters remain value consumers; a later grouping or
alias use must still encounter the typed scalar call's width check and actual
descriptor collation resolution. Lowering registers private descriptors only
for callable query regions, so an unused nested projection cannot demand a
temporary comparator or resource.

### 4. Close reviewed scopes in prerequisite order

The coverage-instrumented sanitizer campaign reproduced a compiler stack
overflow on the accepted 998-term nested ON boundary. Keep the native depth
contract and existing bytecode order. Separate typed eager value emitters
from the canonical expression dispatcher so every recursive activation does
not retain unrelated cursor/call/comparison emission temporaries. This is a
behavior-preserving compiler-frame refactor, not a smaller SQL limit, larger
process stack, new evaluator, or alternative expression algorithm.

Review the ownership refactor and native audit before implementation of new
SQL behavior. Each executable admission starts with a public-session red and
adds scoped binder, planner, lowering, normal/boundary/error/failure/OOM,
independent model, and native differential evidence before publication.

Do not accept a parser prefix while execution or scope semantics are still
unmodeled. Correlation, outer aggregate ownership, IN, FROM-derived sources,
and coroutine consumers remain explicitly staged when their prerequisites
are not yet complete. No intermediate ownership checkpoint completes the
advanced-SQL node.

### Closure evidence

The admitted scalar and EXISTS surface uses the canonical query arenas,
physical producers, verified private read calls, and statement lifecycle.
Public-session cases cover first-row/empty scalar results, one-row demand,
OFFSET and reset/rebinding, nested metadata, ignored EXISTS values and
ordering, delayed aggregate errors, lazy calls, and private-source and PMA
failure recovery. Independent range models exercise 192 bindings. Nested
prepare and scalar-ordering, grouped, and EXISTS set execution cover their
complete measured allocation-failure ranges.

The final fast tier passes 1,272 unit cases. Complete Release and ASan/UBSan
tiers pass 1,318 each; GCC 16 Debug and Release tiers pass 1,316 each. The
30 affected capability, VFS, and B-tree fixture cases additionally pass in
focused Release, ASan/UBSan, and GCC 16 Debug runs after the portability
corrections.

GCC 16 Release initially diagnosed fixture-internal vector insertion,
optional payload initialization, and unchecked byte edits. Shared capability
fixtures now assemble the caller and callee in final instruction order and
derive their region addresses once. Backend-failure fixtures are explicitly
value initialized, and page edits use the existing checked byte-slice and
encoder contracts. These test-only corrections retain the same valid and
malformed program/page inputs; compiler warnings remain errors, and no
production semantics or compiler policy is weakened.

Forty-four new pinned native comparisons and one correlation boundary replace
the retired executable-subquery boundary. All 262 other prior corpus and
oracle records remain identical as JSON data. The published matrix contains
301 comparisons and six unsupported boundaries. The public-session nested
JOIN regression accepts 998 ON terms and rejects 999 without reducing the
native SQL limit.

The grammar-aware ASan/UBSan campaign completes 20,000 inputs with seed 84.
All 58 existing performance-tool, archived-baseline, CLI, diagnostic-work,
and fixture contracts pass. Archived workloads, thresholds, allocation
budgets, and timing baselines are unchanged; this slice makes no timing
speedup claim. Pinned clang-format 23.1.2, changed-production-translation-unit
clang-tidy, engineering discipline, and dependency-graph validation pass.
Correlation, outer-owned aggregates, mutation query destinations, IN,
FROM-derived sources, and coroutine consumers remain separate work.

Incremental GPT-5.6 Sol design and implementation review reports no unresolved
in-scope issue.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 nested-query delivery.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0056,
ADR-0060, ADR-0061, ADR-0072, ADR-0075, ADR-0081, ADR-0082, and ADR-0083.
