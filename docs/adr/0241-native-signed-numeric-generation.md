# ADR-0241: Native Signed Numeric Generation

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #315 main `0af51d63`, ledger325
- Related contracts: ADR-0106/0111/0196/0206/0207/0215/0231/0237/0238/0240

## Requirement and completed bounded mapping

The shared value emitter recognizes original numeric operands but still
loads their positive materialized values and emits runtime negation. That
loses the distinction between an INTEGER token and its positive REAL value.
It also turns a signed hexadecimal generation error into a successful REAL
result. ADR-0207 retained the missing origin; ADR-0215 supplied the checked
signed materializer. This integrates those existing contracts at actual
numeric value-generation requests, not in the generic VM arithmetic owner.

Thirty-two fresh controls on qualified main compare pinned SQLite with the
current read/write facades. Seventeen complete protocols match. Nine cases
have different preparation outcomes, four signed-decimal alias cases differ
in rows, and two retain diagnostic differences. All admitted SELECT controls
agree between the read and write facades; mutations are prepared/finalized
without stepping, and the owned fixture remains unchanged.

For example, `SELECT 9223372036854775808 AS x FROM items WHERE
typeof(-x)='integer'` must return the original positive REAL result row:
native signed generation of the INTEGER token supplies INTEGER INT64_MIN to
`typeof`. Modern currently supplies REAL and returns no row. The same issue
is observed through a syntax-normalized positive prefix, a captured alias
and grouped HAVING. Direct, parenthesized and alias uses of the hexadecimal
minimum instead require a generation-time literal error.

The twelve-case optimizer/lowering preparation baseline has eight samples
per domain, 192 stable observations, and no retained allocation after owner
retirement. It includes ordinary scans, small integers, zero signs, decimal
and REAL aliases, COLLATE, text, columns and parameters. This is allocation
and generated-work evidence, not a timing or speed claim.

| Responsibility | Canonical source |
| --- | --- |
| INTEGER small-value construction versus retained spelling | Pinned `sqlite3.c:185153-185165` |
| Immediate unary-plus construction normalization | `sqlite3.c:185332-185345` |
| Alias replacement preserves the copied root and COLLATE | `sqlite3.c:110495-110535` |
| Target-context selection before value dispatch | `sqlite3.c:117846-117879` |
| Direct numeric sign versus general zero/subtraction | `sqlite3.c:118169-118192` |
| Original INTEGER conversion, minimum and hexadecimal error | `sqlite3.c:117223-117256` |
| Original REAL conversion and sign bits | `sqlite3.c:117205-117214` |
| Wrapper dispatch without rewriting the parent operand | `sqlite3.c:118435-118455` |
| Original eager VALUES generation before later consumers | `sqlite3.c:139901-140009,184755-184763` |
| Existing construction normalization | `internal::UnaryConstructionOperand`, ordinary binder continuation |
| Literal kind/span and borrowed token publication | `StatementBinder::BindLiteral`, `AppendLiteralToken`, ADR-0207 |
| Canonical template copies and alias targets | `BindExpressionTemplateCopy`, `BindResultDefinitionCopy`, `BindAliasReference` |
| One immutable SQL/expression owner for reads and mutations | `BoundSelect::Impl`, `StatementBinder::MoveCommon`, `PlanLowerer::QueryOwner` |
| Bounded nonnegative generated-integer identity | `BoundExpressionIntegerLiteralValue` |
| Checked signed conversion and typed cause | `MaterializeBoundLiteral`, `PlanLoweringErrorCode::LiteralMaterialization` |
| Contextual representatives and iterative generation | `SelectExpressionEmitter`, `BeginValueEmission`, ADR-0231 |
| Existing constant ownership and publication | `StatementLoweringState::literal_constants`, `AddConstant`, `EmitExpressionPayload` |
| Requested producer-eager values versus unrequested query bodies | `EmitValuesExpression`, `EmitReadSubroutine`, `EmitProjectionValue` |

Template copies bind the original syntax again against the same immutable
statement owner; they do not substitute the alias-use span for a numeric
token's span. Ordinary alias wrappers retain their target. Mutation query
owners exist even without a nested query, so the same source view and
expression arena cover INSERT, UPDATE and DELETE.

## Decision

### Integrate sign at the original numeric generation site

Change the existing alias-only numeric classifier to return its canonical
operand ID, avoiding another alias walk. Retain its exact admission:
INTEGER/REAL token descriptors, materialized literals carrying numeric token
kind, and actual literal payloads admitted by the bounded nonnegative integer
fact. Do not look through semantic unary-plus, COLLATE, likelihood, casts,
parameters, columns or function roots.

After the existing contextual emitter selection, a negative expression with
an admitted numeric operand generates its signed constant directly. For a
borrowed token descriptor, call `MaterializeBoundLiteral` with the negative
flag. For a materialized numeric literal, construct a transient descriptor
from its retained kind and the exact checked slice of `QueryOwner()->source()`.
Use that same bridge. Missing source ownership or an invalid range is an
explicit internal-invariant error, never a fallback to runtime arithmetic.

For an admitted generated nonnegative32-bit integer without token provenance,
use the existing bounded integer fact and safe signed integer construction.
Do not treat boolean keywords, arbitrary numeric storage classes or the
already-folded negative decimal minimum as new positive token roots.

Keep the current construction-time normalization. Thus `-(+x)` may refer
directly to an alias's numeric target, while an alias whose definition is
`+number` still has a nonliteral operand and uses general subtraction.
COLLATE and the folded negative minimum retain the same distinction.
Do not change the generic VM negation/subtraction operations.

### Reuse publication, ownership and errors

Publish the signed value through the current program constant builder and
statement-owned literal-constant vector. Index the signed entry by the
negative expression's ID, not its positive operand's ID; aliases may still
need that original positive value. Repeated emission of the same signed
expression reuses that constant, not a runtime value or once site.

Reuse one materialization/error adapter for ordinary borrowed literal
emission and signed numeric emission. Preserve the typed `BindError`, source
span, detail and existing session translation. Do not add a parser, owning
string, origin table, constant container, opcode or value-evaluation cache.
Ordinary eagerly published positive constants remain unchanged in this
slice; removing them would be a different generation-ownership change.

Read and write sessions retain their distinct `read lowering failed with`
and `plan lowering failed with` diagnostic prefixes. Account for these exact
raw contexts separately; they are not complete native-protocol equality.

Contextual representatives must still be selected before the negative root's
ordinary emission. Within a direct numeric negative expression, generate
from the original child token instead of loading a grouped representative
of its positive child. That follows native immediate-child dispatch and
must preserve grouped/captured controls.

The resulting immutable program owns its constants and no borrowed SQL token.
Bound/syntax/caller retirement, program moves, reset and concurrent VM use
must preserve values and sign bits. Typed failures publish no program.
Allocation failures retain the existing compiler exception boundary and
session containment; do not introduce a catch or a success-shaped value.

### Preserve unclosed generation boundaries

This corrects numeric generation only where the current canonical emitter
requests the expression, including retained zero-input read and mutation
bodies. It does not activate a second traversal of every bound expression.

The originally labeled discarded-constructor control
`SELECT EXISTS(VALUES(-0x8000000000000000),(1))` still requests its
producer-eager VALUES expressions. `EmitValuesExpression` generates them
without the ordinary EXISTS projection omission, so this correction also
produces the native literal error there.

Genuinely unrequested constructors remain a separate scheduling gap. Fresh
contrasting controls retain native-error/Modern-statement outcomes for
`SELECT 1 WHERE 0 AND EXISTS(VALUES(-0x8000000000000000),(1))` and
`SELECT EXISTS(SELECT EXISTS(VALUES(-0x8000000000000000),(1)))`.
Logical pruning or an ignored ordinary projection does not request the
inner query body; the existing once-site completion does not synthesize every
unused original producer. This distinction corrects the proposal's overly
broad unconsumed label, not the actual-site scope or emission mechanism.

Competing scalar-width/name/value errors retain their current owners. The
mixed negative-hex/scalar-width observation is not fixed by an early literal
scan. Preserve raw diagnostic differences rather than normalizing them into
complete protocol equality.

Do not add reusable/fixed request modes, FIFO deferred initialization,
unused-producer execution, synthetic-zero reuse or error accumulation here.
The nine deferred-callback differences remain open. No broader generation,
IN, node43 or later-node completion is claimed.

## Test-first proof and delivery

Obtain independent design acceptance, then add one owning behavioral
regression for signed decimal alias typing and record its actual failure.
Implement the smallest shared emitter correction before expanding the
focused boundary controls and running the complete fast unit tier.

Cover decimal minimum and nearby REAL fallback, hexadecimal limits/errors,
underscores, REAL zero/subnormal signs, nested signs, alias construction and
COLLATE barriers, captured/grouped use, borrowed tokens and generated integer
controls. Verify typed error causes and token spans, retained zero-input
bodies, ordinary pruned controls and INSERT/UPDATE/DELETE callers.

Use the existing lowering allocation-prefix owner for the new successful
materialization paths and a signed literal-error path. Extend its expected
result check only for that explicit typed failure; preserve every existing
successful baseline, failure prefix, recovery, ceiling, worker and deadline.
Update its exact population and the dependent workflow-policy assertion in
the same source. Keep generic runtime arithmetic, source lifetime, reset,
statement cleanup and all retained generation controls covered.

Repeat the bounded protocol and matched preparation observations against the
actual candidate. Account separately for corrected outcomes, raw diagnostic
differences and the known unconsumed/width/deferred gaps. Report any added
allocation/constant work without making an unmeasured optimization claim.

Retain focused red/green, complete fast evidence, independent source review,
normal commit/ledger traceability and the actual change-selected closing
plan. Required compiler/checker, hosted, integrated and separate actual-main
gates remain unchanged. Deliver this coherent node43 correction before
starting the next general generation prerequisite.
