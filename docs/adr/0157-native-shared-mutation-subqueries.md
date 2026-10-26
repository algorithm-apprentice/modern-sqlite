# ADR-0157: Native Shared Mutation Subqueries

- Status: Accepted
- Date: 2026-10-26
- Module graph node: `implement-advanced-sql`
- Delivery: bounded shared-query prerequisite for ADR-0053 slice 8

## Context and completed source mapping

Start from PR #260's accepted main `3e44d30`, tree
`8d195e9e450146c1a7c37d2751766073a2c2f492`. Native Windows admission is
complete; node 43 is not. The next IN query-RHS delivery must use one compiler
and execution path in read and write statements.

The existing `StatementBinder` already binds all statements, but its mutation
publication moves only `BoundExpressionState`. It discards the SELECT-owned
query, aggregate and core arenas and rejects every mutation subquery before
binding it. Logical/physical query views and `PlanLowerer` likewise assume a
read-statement owner. Simply removing that rejection would leave dangling
query identities, omit nested register layouts and lose write-effect metadata.

This prerequisite admits uncorrelated scalar and EXISTS expressions in the
currently supported INSERT values, UPDATE assignments and UPDATE/DELETE
predicates. Their SELECT/VALUES, aggregate, compound, join, ordering and
inner-local correlation behavior is the already admitted read-query surface.
References from a query to the mutation target remain an explicit unsupported
boundary, including binding-only references in an ignored EXISTS result.
No capture may be treated as uncorrelated merely because its value is ignored.

IN SELECT/VALUES RHS, promoted singleton SELECT, row values, named IN
shorthand and IN-driven WHERE loops remain unsupported here. Their complete
native existing-index/rowid, materialization, signature-reuse and Bloom paths
were researched before selecting this prerequisite, but are not replaced with
a scalar LIMIT-1 destination or a simplified membership algorithm.

The source oracle is the local pinned SQLite 3.54.0 checkout:

| Responsibility | Pinned source and mechanism |
|---|---|
| Lexical resolution and correlation | `resolve.c:1412-1438`: resolve a SELECT in the enclosing name context; retain outer-reference changes as `EP_VarSelect` and retain `NC_Subquery` separately |
| Shared scalar/EXISTS execution | `expr.c:3914-4050`: one subroutine, NULL/zero initialization, implicit LIMIT 1, native existing-limit normalization and reached-once caching only without free outer references |
| SELECT destinations | `select.c:1464-1498,3268-3310`: the same SELECT producer publishes a scalar value or EXISTS flag; no write-specific query executor |
| INSERT value ownership | `insert.c:1413-1428`: evaluate through the ordinary expression compiler and copy a subquery value before later expression/mutation work |
| UPDATE name binding and rightmost ownership | `update.c:480-503`: resolve every RHS before its target name; the column/rowid map retains only the rightmost assignment for value generation |
| UPDATE scan admission | `update.c:735-788`: WHERE subqueries disable multi-row one-pass; a proven single-row lookup remains eligible; collect rowids before the update pass |
| Clause/pass-sensitive planning context | `where.c:6926,7207,7902`; `update.c:813-817,1136-1140`: WHERE compilation adds the selected output estimate, while WhereEnd restores it before point/two-pass assignments but after multi-row one-pass assignments |
| UPDATE assignment evaluation | `update.c:889-978`: read old fields and evaluate changed values through the ordinary expression compiler before writing the new record |
| DELETE scan admission | `delete.c:438-448,495-588`: retain `NC_Subquery`, disable multi-row one-pass and collect rowids before deleting; retain the single-row exception |
| Private resource boundaries | Existing ADR-0082/0083/0090 and `vdbe.c` subroutine/cursor cleanup mappings: read callees never borrow or mutate caller cursors |
| Error completion | Existing ADR-0119/0136/0151: compiler-owned multi-write/ABORT effects, typed VM failure and the canonical statement/transaction coordinator |

Sixty-five source-attested native public-API controls were recorded before design.
The mutation controls confirm cached self-reads, INSERT publication,
WHERE-query row collection, ignored EXISTS value errors, nested query-local
correlation, scalar width errors and rollback after a late assignment error.
For example, with values 10/20/30, inserting `(SELECT max(v) FROM u)` then
running `UPDATE u SET v=(SELECT max(v) FROM u)+id` yields 31/32/33/34, not a
new maximum on every row. A later overflow leaves all earlier row changes
rolled back. The IN controls also distinguish promoted SELECT from scalar
values, ascending/descending stored access, NULL/affinity/collation, alias
signature reuse, SELECT ALL separation and Bloom eligibility. These are
correctness/fixed-work observations, not a timing or feature-parity claim.

The five clause/pass controls use the same ten-row analyzed inner join.
Native creates one automatic index for a multi-row one-pass UPDATE assignment
and for a WHERE query, but none for an INSERT value, point UPDATE assignment
or two-pass UPDATE assignment. Once-caching alone does not authorize replacing
that planning context with zero.

## Decision

### 1. Publish one canonical query/expression owner

Reuse `BoundSelect`'s existing immutable implementation as the complete
statement expression/query owner retained by each mutation. Keep its existing
public name in this bounded change; do not add another query arena, query
binding cache, aggregate owner or common-value representation.

The binder already uses query identity zero as its enclosing statement
context. In a mutation it is a lexical anchor, not a fabricated SELECT body.
Only nonzero identities have logical/physical query bodies. Publish the
existing owner by move, without cloning expressions, source metadata or query
cores, and forward mutation expression/parameter/catalog access through it.
Borrowed catalog names remain protected by that same retained snapshot.

Initialize the existing query binding/core arenas for statements that contain
queries. Query-free mutations retain their existing allocation path and
exact allocation contracts; no eager query-frame or plan allocation is added.
The statement-global IDs, existing alias-copy rules, parameter assignment,
typed errors, scope save/restore and aggregate ownership remain canonical.

After ordinary binding, reject a direct mutation query whose transitive
capture list is nonempty. Preserve name-resolution and aggregate-legality
errors before that unsupported boundary; section 2a assigns UPDATE's
value-only checks to their actual consumers. Nested queries
may capture their query-local ancestors exactly as ADR-0090 already permits.

### 2. Borrow the same logical and physical query views

Make current query views borrow the canonical bound owner and their existing
query-plan record, rather than requiring an independently owning read
statement. Read-plan construction and mutation child-plan construction both
invoke the same query builder and optimizer. Mutation plans own only their
existing nonzero query-plan records beside the bound statement.

Default, moved-owner and out-of-range views remain inspectable through
`valid()`. Metadata access requires a valid view and checks that invariant
before dereferencing borrowed pointers. An invalid access terminates, as
other impossible published-plan states do; it does not return empty metadata
or require every consumer to repeat the pointer checks.

Do not synthesize a root SELECT, copy a bound query, recursively prepare SQL,
or add a write-side planner/lowering implementation. Preserve parent-before-
child planning, ordinary read options and descendant invocation estimates.
Record direct mutation-query invocations through the current expression-loop
propagation, with the context at their native clause/pass:

- INSERT values and assignments after point/two-pass WhereEnd use the saved
  initial context;
- WHERE query sites use the active selected mutation WHERE output estimate;
  and
- assignments in a multi-row one-pass scan retain that active estimate until
  the loop ends.

For the currently admitted mutation access family, reuse the existing
LogEst conversion/table cardinality for scans and the existing rowid-shortcut
output for point access. A single-table unordered scan does not apply the
read join/ORDER BY output adjustment (`where.c:3042-3047`). Keep the existing
parent mutation access/scheduling choices; do not add a second costing model
or broaden writer access paths. Descendants use the same canonical query
propagation as read statements. Test the five native automatic-index
controls above rather than inferring correctness from equal cached results.

### 2a. Preserve UPDATE's name-binding and value-generation phases

Implementation review identified that the inherited UPDATE emitter evaluated
overwritten RHS expressions even though the binder already marked them
ineffective. Thirteen additional executed native controls close this source
mapping before correction; they are not permission for a query-only skip.

Use a pure lookup through the existing mutation-field resolver to populate
the existing assignment slots and establish normalized rightmost ownership.
Defer an unknown target's diagnostic until after its RHS name binding, rather
than raising a schema-check request during that lookup. No second mapping
container or successful-statement allocation is introduced.

Bind RHS expressions in source order. Effective assignments retain required-
value binding, even with `WHERE 0`; optimizer-pruned and overwritten values
are not the same validation exemption. Only overwritten assignments use the
existing transitive binding-only mode, including nested aggregate arguments;
a scalar unneeded-value flag alone is insufficient. Keep names, function arity, aggregate legality and
compound structure checked for every RHS, then report an unknown target at
its native point.

Tag existing scalar-width and required-collation diagnostics as value
requirements at their canonical producers. If an effective RHS reports one,
retain that diagnostic, restore the existing arena checkpoint and replay
only that erroneous RHS in non-value mode to finish name validation. A later
name error still precedes value generation. Otherwise return the retained
value error in rowid/schema-column order after WHERE binding; never publish
a successful bound statement after such an error. Successful values are
bound only once. This is diagnostic deferral on invalid input, not another
validator, query representation or normal-path rebinding pass. Existing
lowering/VM checks remain defenses, not substitutes for required preparation
validation. INSERT retains its distinct duplicate-target input evaluation.

Retain the current source-ordered assignments and canonical normalized
rightmost-effectiveness facts. Emit only effective values: the new rowid
first, its exact-integer coercion immediately afterward, then ordinary
columns in schema order. Reuse existing assignment/target metadata and
table-record handling of the rowid-alias payload. Transposing the existing
bounded field lookups keeps the current assignments-times-columns preparation
bound without an additional mapping allocation. Unassigned values retain
the existing source snapshot; assigned payload fields need no preliminary
copy that will immediately be overwritten.

The native controls confirm that overwritten overflow queries/functions,
scalar width-two queries and missing ORDER/comparison collations are ignored,
but unknown names, wrong arity and compound-width structure still fail.
They also distinguish RHS-before-target name errors, rowid-before-column
errors, immediate rowid coercion and schema-column callback order. Tests
must cover those phases and preserve actual-value width/collation errors,
normal INSERT duplicate evaluation and every original allocation ceiling.

### 3. Reuse current layouts, caches, producers and read subroutines

Prepare every retained child frame through `PrepareReadLayout` before the
builder's final resource counts are fixed. Use the same statement-global
register allocator, constant/symbol tables, query cache initialization and
private body emission as an ordinary read statement.

The mutation host supplies the existing write transaction/database. Child
read cursors use that database and must close before private return. They
never use the mutation's positioned cursor. No VM instruction, verifier
relaxation, public session API, second executor or temporary format is needed.

Keep lazy invocation, per-semantic-instance cache identity, NULL/zero initial
values, native LIMIT/OFFSET behavior, EXISTS projection elision and existing
coroutine cancellation. Reset/rebind, halt, failure, finalize and destruction
retire the complete execution state through existing cleanup.

Mutation ABORT effects are statement-owned, not frame-owned. Scalar calls
emitted in child frames must contribute through the same existing compiler
effect mechanism as calls emitted directly in a mutation. Retain
`uses_statement_journal`, multi-write classification and coordinator policy.
Do not conservatively mark every query abortable or introduce savepoints for
ordinary error-free reads merely because they cross a private boundary.

### 4. Preserve native mutation scheduling

Extract/reuse one canonical bound-expression query-presence traversal for
current lowering and mutation planning. It covers aliases, scalar wrappers,
function arguments and membership operands without evaluating expressions.

For a table-scan UPDATE or DELETE whose WHERE expression contains a retained
subquery, select the existing original-rowid collection path. A rowid point
lookup retains the one-row path. Assignment-only subqueries do not by
themselves disable multi-row one-pass. Query-free statements keep their
existing choices. Reuse the admitted rowid collector; do not add another
FIFO/set algorithm or materialize entire old rows.

This phase distinction is required even for an uncorrelated cached query:
equal final rows are not sufficient evidence for a different mutation order.

## Focused red/green and delivery

Deliver one coherent, reviewable vertical slice after independent design
review. Begin with a public WriteSession regression that currently fails
because mutation subqueries are rejected. Record red before changing
production, then run focused green and the complete fast unit tier.

Owning tests cover:

- one bound owner, stable IDs after moves, lexical parents, shared parameters,
  nested local captures and explicit mutation-target capture rejection;
- scalar/EXISTS width, name, aggregate and LIMIT/OFFSET errors;
- the same read-query logical/physical nodes and private bytecode lifecycles;
- scan two-pass versus point one-pass and assignment-only one-pass controls;
- query execution once per statement, lazy skipped queries, empty scalar NULL,
  ignored EXISTS projections and reset/rebind;
- both temporary modes, source/index/ordering/aggregate/compound consumers;
- late typed errors, retained explicit transactions and statement rollback;
- focused binder/lowering/runtime allocation injection, without changing old
  allocation ceilings, fault prefixes, workloads or deadlines; and
- pinned SQLite write interoperability and integrity after shared-query DML.

Use ADR-0102's actual-diff verification plan, native MSVC and the required
GNU/sanitizer/hosted selections. Review the implementation and exact delivery
head independently before guarded squash merge and associated branch cleanup.
Compatibility-first ADR-0053 defers new timing families, repeated benchmark
runs and optimization; none is claimed as passed or used to authorize a
different native mechanism.

Append the exact source/review/red/green/fast/closing evidence to the existing
append-only node-43 ledger. Update the portable handoff with the admitted
boundary. This prerequisite does not admit IN query RHS, complete node 43,
or begin a later graph node.
