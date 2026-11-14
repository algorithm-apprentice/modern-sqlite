# ADR-0263: Non-Descending Comparison Name Resolution

- Status: Accepted
- Date: 2026-11-14
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #335 main `686478ca`, ledger421
- Related contracts: ADR-0090/0092/0106/0111/0192/0251/0252/0261/0262

## Observable boundary and existing implementation

PR #335 delivers source preparation before expression names. Compound ORDER
trials still call full `BindExpression`, descending into child SELECTs that
native resolves with `NC_NoSelect`. Suppressed trial errors do not undo
schema-check requests, so this changes subsequent cached-schema behavior.

Fresh observations on qualified main retain the existing warm-schema
observer, pinned reference, fixture replacement, before-step metadata query
and both session facades. The eighteen-case matrix has nine differences:
six unnecessary schema refreshes and three lost real-name refreshes.
Seventeen additional function/modifier/membership/context controls contain
thirteen differences. One is an IN query-RHS parser rejection, retained as
an unsupported-feature control rather than a selected repair. The other
thirty-four observations contain twenty-one admitted comparison differences.
This is a bounded design matrix, not the total remaining node43 workload.

Skipping every query-containing expression is incorrect: ordinary sibling
names must still resolve. Returning an error at the first child query is
also incorrect. Native function callbacks can continue enclosing traversal
after a nested name failure. Examples include an invalid nested function
followed by a missing sibling, and invalid likelihood metadata followed by
argument-name resolution.

The current private `BindingStep` has only a real `BoundExpressionId` or a
child request. Leaf dispatch and the expression loop therefore cannot
represent an intentionally unvisited SELECT without manufacturing a public
query/value or aborting the rest of the expression. The selected change
extends this existing owner; it does not add a second namespace or AST.

## Completed selected source map

Native locations refer to the manifest-pinned `sqlite3.c`.

| Responsibility | Native / current owner |
| --- | --- |
| Trial name scope, diagnostic suppression, then comparison | `resolveOrderByTermToExprList:111989-112035`; `MatchCompoundOrderByExpressions` |
| No SELECT callback, full root-height check and scoped flags | `sqlite3ResolveExprNames:112623-112655`; `BindExpression` state restoration |
| SELECT/EXISTS encounter without descent | `resolveExprStep:111838-111865`; `BindSubquery` is the current excessive descent |
| Suppressed diagnostics versus schema requests | `sqlite3ErrorMsg:37060-37085`, `lookupName:111183-111297`; `SchemaLookupError` |
| Function diagnostics and child-walk boundaries | `resolveExprStep:111575-111835`; existing function/aggregate continuations |
| Converted ORDER name context | `resolveSelectStep:112368-112400` moves ORDER back into the child compound for resolution |
| Alias replacement and original identities | `resolveAlias:110495-110534`; `BindAliasReference`, `ResultDefinition`, template-copy scope |
| Query-containing expressions do not compare equal | `sqlite3ExprCompare:119465-119554`; `NativeExpressionsEquivalent` |
| Ordinary/eager source scopes and trial retirement | ADR-0251/0252; `NameScopeView`, `SourceBindingScope`, `BindingArenaCheckpoint` |
| Existing iterative expression owner | `BindingFrame`, `BindingStep`, `SelectBindingRoot`, `BindExpressionLoop` |

`NC_NoSelect` disables the SELECT callback, not the expression callback.
Native still records SELECT/EXISTS encounter flags, validates the complete
expression's construction height and resolves names outside those SELECTs.
The converted-compound source walk differs, but name resolution moves ORDER
back into the child compound; converted ORDER is not a reason to re-enable
SELECT descent during these trials.

Under diagnostic suppression, ordinary `sqlite3ErrorMsg` does not increment
the parse-wide error count. The name context separately remembers its errors.
An unresolved identifier sets `checkSchema`, increments the name-context
error count and returns `WRC_Abort`. A function callback records its own
diagnostics, walks its argument list, and does not propagate that list's
abort to the enclosing expression. Valid aggregates also walk local ORDER
and FILTER; scalar/unknown/wrong-arity functions do not resolve modifiers as
aggregate modifiers. The callback then returns `WRC_Prune`.

This is why a trial needs both a remembered failure and explicit traversal
control. Clearing the schema flag, catching every failure and continuing
everywhere, or stopping all siblings after a query would each be wrong.

## Decision

### One private completion model in the existing binder

Add an explicit comparison-name purpose to the private binding scope.
Ordinary binding remains the default. Extend the existing internal
completion carrier to distinguish a real bound expression, a SELECT
boundary deliberately not materialized, and a suppressed name-failure
completion. These are control outcomes, not SQL values or public IDs.

Keep the public bound model and ordinary `BindExpression` result unchanged.
Factor its existing cached-construction/root-height/state-restoration work
so ordinary and comparison entry points use the same mechanism. Ordinary
completion must contain a real expression; a comparison-only completion
escaping into an executable caller is an explicit internal invariant error.
Specialize the shared entry/loop by purpose without adding retained wrapper
frames to ordinary recursive query binding; preserve its existing depth bound.
Do not supply ID zero, NULL SQL data, an unresolved public query, or a new
public expression variant.

Use the same `BindingFrame`/continuation stack and canonical leaf/name,
function-classification and modifier helpers. Introduce no second recursive
name walker, source-name index, expression arena, plugin policy or syntax
copy. A SELECT boundary carries only immutable syntax identity/kind needed
to account for the encounter; it owns no executable metadata.

### Preserve native comparison name-walk control

In comparison purpose, scalar SELECT and EXISTS nodes do not invoke
`BindSubquery`. Preserve the root height check and private query-encounter
facts, then complete that child without a value. Continue ordinary parents
and siblings in native name-resolution order. Constructor-deleted syntax
still uses existing normalization and remains absent.

Remember expected name-resolution diagnostics in a private per-trial
context while preserving their actual schema-check effects. A missing or
ambiguous identifier aborts the current walk/list. The nearest function
callback handles that abort as native does: stop that child list, retain
the failure, visit any still-required aggregate modifier lists, then allow
the enclosing expression to continue. Without such a function boundary,
propagate the abort to the trial root.

Function name/arity, aggregate permission, scalar-modifier and likelihood
diagnostics must not become premature whole-expression exits. Reuse their
canonical validation and classification, but defer the diagnostic according
to native callback control. Likelihood name resolution visits its original
argument list even when probability validation failed; its ordinary runtime
operand/value path remains unchanged.

Use original syntax/effective argument counts for modifier traversal and
arity checks. A query child with no bound value must not make a nonempty
aggregate look like a zero-argument aggregate. Zero-argument function ORDER
remains discarded; scalar modifiers remain outside aggregate name walking.
Do not build a partial runtime argument list or aggregate invocation from
only the surviving children.

Comparison is a name-resolution phase, not value generation. Suppress its
value-only literal/collation/demand work through the existing binding
requirements. Complete ordinary subtrees with their real canonical bound
forms only when their children have real values and name resolution permits
it. Otherwise retain the private non-value completion. A failed trial or
one containing an unmaterialized query cannot produce a column match.
Clean query-free trials still use the single existing native comparator.

Keep fatal/internal failures and allocation unwinding distinct from expected
suppressed name diagnostics. Do not introduce a broad exception catch or
silently convert an invalid ownership state into a successful expression.

### Alias and scope ownership remain canonical

Whole-name/ordinal matching retains ADR-0251 encounter order and fast paths.
For an alias reached inside a comparison expression, preserve the existing
definition/consumer aggregate checks, source precedence, DQS/boolean/rowid
rules and capture validation.

An already bound query-bearing result definition must not be rebound merely
to copy a trial expression. In this comparison-only context, append the
existing canonical alias reference to its real original target instead of
creating another template/query instance. Its target already owns real
query/source identities; the comparator retains its nonmatching query
semantics. Do not mutate that target or overwrite its original syntax
provenance. Executable alias consumers keep ordinary template copying.

Ordinary SELECT, ordinary VALUES and ADR-0252 eager namespaces share the
same policy. Do not infer an eager source from absence of a catalog table,
change fresh outward-name visibility, or attach an executable table/query
owner to the comparison scratch.

`ParseInExpression` rejects SELECT/VALUES query RHS at
`src/syntax/parser.cpp:1074-1076`. Preserve that parser rejection and the
existing unsupported binder boundary for such public syntax. Do not admit
IN query RHS even for an ORDER trial. Scalar-list IN can already contain
admitted scalar SELECT expressions; those children follow this decision's
ordinary non-descending completion policy.

### Keep retirement and other phases unchanged

Retain the existing comparison arena checkpoint, source-type-work
suppression, borrowed name view and restoration order. Only a selected
output-column index may escape. Clear per-trial diagnostic/control state
before the next core/term, but never clear statement-level schema requests.
Retire temporary expressions, function/collation registrations, captures
and any existing eager scratch through their current owners.

Do not rewind or rebuild ADR-0262 prepared-source entries: their immutable
metadata is independent of these name trials. Persistent catalog/SQL state,
public query representation, ordinary value-generation order and later
nodes do not change.

## Focused proof and delivery

After independent design acceptance, add a focused failing owning test for
the current query-descent/schema-effect differences before implementation.
Use both binder and session facades. Preserve the observed normal/boundary
controls and their exact schemas, not just the final ORDER-error category.

Cover query roots and ordinary parents, EXISTS, COLLATE, eager scopes,
function/likelihood diagnostics, aggregate arguments/ORDER/FILTER, scalar-list
membership query children, copied query aliases, converted compounds and
later-core name misses. Include discarded-constructor and query-free
matching controls so the change cannot become an expression-presence
shortcut or weaken ordinary execution.
Keep the existing accepted/rejected query-copy depth cases unchanged.

Extend the existing allocation-prefix owners for the new continuation
outcomes, failure continuation and alias borrowing. Preserve previous cases,
ceilings and deadlines. Reuse the matched allocation/lifetime observer and
report any changed frame storage or temporary allocation schedule without
claiming a speedup. Keep instrumentation outside production.

Run focused green, complete fast units, unchanged native/warm-schema
controls and the actual change-aware closing union. Require source review,
per-commit node/ADR ledger evidence, final-head hosted gates and separate main
qualification. Keep this one coherent comparison correction reviewable;
do not batch unrelated name/LIMIT/value-generation repairs or new SQL
features into it merely because they are also incomplete. Node43 remains
in progress.
