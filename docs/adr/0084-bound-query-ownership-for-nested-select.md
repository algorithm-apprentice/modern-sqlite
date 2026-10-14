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

### 4. Close reviewed scopes in prerequisite order

Review the ownership refactor and native audit before implementation of new
SQL behavior. Each executable admission starts with a public-session red and
adds scoped binder, planner, lowering, normal/boundary/error/failure/OOM,
independent model, and native differential evidence before publication.

Do not accept a parser prefix while execution or scope semantics are still
unmodeled. Correlation, outer aggregate ownership, IN, FROM-derived sources,
and coroutine consumers remain explicitly staged when their prerequisites
are not yet complete. No intermediate ownership checkpoint completes the
advanced-SQL node.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 nested-query delivery.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0056,
ADR-0060, ADR-0061, ADR-0072, ADR-0075, ADR-0081, ADR-0082, and ADR-0083.
