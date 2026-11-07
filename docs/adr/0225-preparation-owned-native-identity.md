# ADR-0225: Preparation-Owned Native Expression Identity

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #302 main `bdf750ae`, ledger267
- Related contracts: ADR-0033/0036, ADR-0106/0111, ADR-0207/0215 and ADR-0224

## Current requirement and completed owner inventory

The remaining native generation owner needs exact resolved-expression
identity after binding. Native `sqlite3ExprCodeRunJustOnce` searches its
already queued reusable expressions with `sqlite3ExprCompare(...)=0`.
SQL-value equality, an expression arena ID and a generation-site identity
are not interchangeable.

ADR-0224 corrected the current binder consumers using one iterative
comparison owner and a private syntax-origin table. The table and its AST
currently die when `StatementBinder` returns. Published expressions retain
resolved symbols and numeric token provenance, but special-function names,
scalar DISTINCT and likelihood probability spelling are not all represented
by their lowered payloads. Reimplementing comparison in lowering, reparsing
the SQL, interning every possible expression in advance, or constructing a
second syntax tree would introduce a competing mechanism.

The existing common owner already serves reads and writes:

| Responsibility | Pinned or current source |
| --- | --- |
| Exact native identity and conservative SELECT rejection | `sqlite3.c:119465-119553` |
| Search only earlier reusable entries before once/deferred classification | `sqlite3.c:118698-118740` |
| Temporary versus requested-destination generation | `sqlite3.c:118778-118906` |
| Prior-error barrier and deferred queue drain | `sqlite3.c:126820-126950` |
| Expression cleanup at preparation retirement | `sqlite3.c:148915-148934` |
| Zero-argument ORDER attachments are discarded during parsing | `sqlite3.c:114029-114064` |
| Current comparison and pending-aggregate adapter | `bound_select.cpp:6395-7000` |
| Owned common bound arena | `BoundSelect::Impl` |
| Mutation reuse of that same owner | `MoveCommon`, `MutationQueryOwner`, `BoundStatementQueryOwner` |
| Canonical qualified-name and parenthesis helpers | `NameParts`, `Dequote`, `SpanText`, `ParenthesesTransparentExpression` |
| Immutable syntax storage and default move behavior | `syntax/ast.hpp::SyntaxTree` |
| Current duplicate SQL copy and deferred-token borrowing | `InitializeCommon`, `AppendLiteralToken`, `BoundSelect::source` |
| Preparation artifacts do not enter statement execution | `read_session.cpp:180-229`, `StatementExecution` |
| Existing allocation-error boundary | `ReadSession::Prepare`, shared preparation helpers and mutation preparation |

`SyntaxTree` contains an inline `std::string`, expression arena, statement
and query arena. Moving it after publishing borrowed token views is unsafe
for short-string storage. Its owner must be installed before such borrowing,
not repaired with offsets after a late move.

The remaining signed-alias controls were refreshed through both read/write
facades on source263. Their eleven outcomes remain unchanged from source249.
Native deferred errors still depend on the actual generation barrier.
This identity-owner prerequisite does not repair or waive those differences.

## Decision

### Extract one borrowed comparison view

Move the existing iterative native comparison kernel into one private
comparison implementation in the binder translation unit. Its context
borrows the current syntax, bound expressions, origin entries, source
columns, aggregate invocations and pending aggregate records. It performs
no binding, value evaluation, bytecode generation or registry mutation.

Keep the existing continuation stack and native comparison decisions,
including alias traversal, resolved source identity, literal spelling,
special forms, signed-minimum shape and conservative subquery rejection.
The existing aggregate-invocation comparison uses the same kernel and its
pending-record adapter during binding; published owners have no pending
aggregate records.

Reuse the current qualified-name, dequoting, source-span and parenthesis
helpers through one private implementation. Do not copy their algorithms
into the comparison class or add callbacks, virtual services or a plugin
interface. The context is a bounded borrowed view, not another arena or
owner. Existing binder consumers retain their exact/root-COLLATE policy.

This extraction is a green refactor. It does not expose a new public
comparison operation or change the provenance lifetime by itself.

### Keep the original provenance in the common preparation owner

Install the original immutable `SyntaxTree` in `BoundSelect::Impl` when the
common owner is constructed, before binding publishes any borrowed view.
`StatementBinder` borrows that stable tree. Move the existing origin vector
into the same common owner rather than creating a second table.

Use the tree's owned SQL text as the canonical source buffer. Remove the
now-redundant common `std::string` copy and direct deferred token slices and
the common `source()` accessor to the stable source owner. Do not move an
inline string after borrowing from it. DDL outputs that do not use the
common bound owner keep their existing independent source ownership.

Origins still attach at successful binding completion, preserve cached
roots and actual copies, and append/rewind with the bound-expression arena.
Retain canonical reservation and allocation-failure behavior. The original
AST is never rebound, rewritten with register numbers, or copied into a
second identity representation.

The common owner's existing move-only lifetime now owns the immutable
syntax and origins through planning and lowering, or for as long as a caller
retains that bound artifact. Read, INSERT, UPDATE and DELETE use the same
owner and storage. No syntax accessor or syntax ID is exported to a plan,
bytecode descriptor or VM. Executable programs and VM state retain no AST.

This explicitly supersedes only ADR-0224's binder-local provenance-retirement
boundary. Its single comparison owner, origin-index invariant, public
expression layouts and native comparison semantics remain in force.
The retained AST is the already existing parser output, not a new parser,
cache, comparison tree or migration representation.

### Expose exact identity through the existing bound owner

Add a binder-level `NativeBoundExpressionsEquivalent` operation over one
`BoundSelect` and two `BoundExpressionId` values. It returns
`std::expected<bool, BindError>` and invokes the same comparison kernel with
exact matching. The public operation does not accept a root-COLLATE policy;
that remains an internal requirement of the existing binder consumers.

Validate the owner and operand IDs at the public boundary. A moved-from
owner or out-of-range operand is a typed invalid-input error, not false
equivalence. Internal origin/arena inconsistencies remain explicit errors.
Existing synthesized nodes keep their documented interpretation; an
arbitrary missing origin must not become a value-only identity match.

Use the existing allocation-exception boundary and typed-error conventions,
not a broad catch, fabricated SQL value or new error family. A caller can
compare only expressions belonging to that owner. Mutation hosts obtain
the same common owner through their existing `query_owner()` accessors.

The operation does not assign generation-site IDs, search or allocate a
deferred queue, decide constant eligibility, or authorize cache reuse across
coroutines. Native requested/reusable destinations and physical activation
ownership remain separate inputs to the future generation owner.

## Ownership cost and validation

Record a matched baseline before production changes. The first extraction
must preserve allocations, public layouts and existing observations.
For provenance retention, distinguish requested allocations from storage
that remains live after binding and from peak memory. Account for the
retained original AST/origin capacity and removal of the duplicate SQL
buffer. Do not infer retained or peak bytes from requested-byte totals, and
do not claim a speed improvement.

Add one focused public-boundary interface red before exposing the operation.
Then prove exact native identity after the input SQL and moved parser
objects have been retired, through short and long source text, bound-owner
moves and the existing shared mutation owner. Cover retained literal and
parameter spelling, special-function distinctions, normalized/signed
forms, valid synthesized nodes, conservative subqueries and invalid IDs.

Preserve all existing SQL rows, diagnostic ordering, callback counts,
aggregate reuse and native identity controls. Exercise speculative rewind,
HAVING copies and complete owning allocation-failure sweeps without raising
their limits. Add no new timing family or production instrumentation.
Changed C++ tests also receive an actual strict Clang/libc++ compile so the
recent fixture warning is not discovered only after publication.

## Bounded delivery

Use two related, buildable source slices under ADR-0106: the private
comparison-view extraction, then common provenance ownership and its public
exact comparison bridge. Each slice requires its own baseline/red-green,
complete fast units, source review and normal commit before the next begins.
Final integrated gates run against the exact batch head.

Independent design acceptance precedes implementation. No later generation
activation is admitted by this document. General ordinary/reached-once/
deferred scheduling, shared eager results, signed generation, error
arbitration, IN, node43 and later DAG nodes remain incomplete.
