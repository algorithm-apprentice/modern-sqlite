# ADR-0207: Native Numeric Token Provenance

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted ADR-0206 source215 `4f359f6a`, ledger215, draft PR #293
- Related contracts: ADR-0033, ADR-0106/0111, ADR-0160 and ADR-0196/0200/0206

## Completed mapping and current requirement

Source215 corrects general unary subtraction without moving ordinary
integer/name/value-generation errors. It is independently accepted and
published; its draft is not final batch delivery. Sixteen retained preparation
observations and a separate signed-integer alias boundary remain open.

The matched alias control
`SELECT 9223372036854775808 AS x FROM items WHERE deterministic_trace(-x)`
exposes a missing prerequisite. Native passes INTEGER INT64_MIN to the
identity callback, while Modern passes REAL negative2^63. The positive result
literal is already REAL in both engines. Its materialized SQL storage class
therefore cannot recover the native INTEGER-token generation rule.

| Responsibility | Pinned source or current owner |
|---|---|
| Preserve FLOAT token kind, spelling and source offset | Amalgamation `sqlite3.c:180777-180808,185149-185151` |
| Distinguish small INTEGER construction from retained INTEGER text | Amalgamation `sqlite3.c:185153-185165`; `expr.c:944-1007` |
| Normalize numeric underscores without changing hexadecimal kind | `util.c:339-370` |
| Copy the expression root during alias replacement | `resolve.c:68-106` |
| Generate INTEGER/FLOAT using the original kind and sign | `expr.c:4408-4463,5372-5395` |
| Own immutable SQL and publish literal values/tokens | `BoundSelect::Impl`, `StatementBinder::BindLiteral`, `AppendLiteralToken` |
| Require each literal span to equal its token span before binding | `src/syntax/ast.cpp:351-361` |
| Preserve bounded native integer facts | `BoundExpressionIntegerLiteralValue` |
| Select existing direct/general unary paths | `PlanLowerer::HasNumericLiteralOperand`; ADR-0206 |

Modern already owns the immutable statement SQL and retains unused literal
tokens in `BoundLiteralTokenExpression`. Successful ordinary numeric
materialization instead publishes only `SqlValue`. An INTEGER token may have
become REAL, and the same INTEGER value may represent either a hexadecimal
token or the existing folded signed-minimum expression.

Do not infer token kind by reparsing a materialized value or by treating every
numeric `SqlValue` as a native numeric token. This source supplies the missing
identity; the general generation/error owner remains a separate correction.

## Decision

### Retain numeric origin in the existing literal carrier

Add an optional `LiteralKind` numeric-token-kind field to
`BoundLiteralExpression`. Reuse the existing enum rather than adding another
numeric type system. The field is present only for successfully materialized
original INTEGER or REAL tokens. It describes source syntax, not the
materialized storage class.

When publishing one of those literals, preserve its existing exact validated
token range in `BoundExpression::span`. The source bytes continue to
belong solely to `BoundSelect::Impl::source`. Do not add a string/view owner,
second arena, AST reference, source-range table or value cache.

`SyntaxTree::Create` already requires a literal expression span to equal its
token span. Preserve that validation and publish the existing span unchanged.
Parentheses are separate syntax nodes, not a wider literal span.
Result-column naming still receives the original outer syntax expression
span. Keep existing lexical validation and failure spans; no new range
normalization or broader public-AST admission is needed.

Keep the field absent for NULL, boolean keywords, TEXT/BLOB, DQS fallback,
generated parser/limit constants and the existing signed-minimum shortcut.
In particular, the shortcut represents an original unary expression, not a
new positive INTEGER-token root. Unmaterialized tokens retain their existing
kind/view representation and ownership contract.

Alias references keep their existing target; template copies run the same
canonical producer. Neither alias sites nor later wrappers invent or overwrite
the target's numeric token kind. Caller/source-tree retirement and moves must
leave the recorded kind and owner-backed token range inspectable.

### Use origin, not storage class, for direct numeric recognition

Update the current private unary classifier after its canonical alias-only
peek. Let that same peek return the canonical bound ID needed by the integer
fact; do not rescan aliases or construct another arena view.
INTEGER/REAL token descriptors and materialized literals carrying
numeric provenance retain the direct path. For a materialized literal without
that provenance, reuse the existing bounded integer-literal fact only for
the actual literal payload, preserving generated nonnegative32-bit integer
constants and boolean exclusion.

Do not call that fact through arbitrary unary wrappers to pretend they are
direct numeric roots. The original signed-minimum shortcut consequently uses
the existing general zero-minus path when negated again. Its value, NULL,
overflow and error behavior remain unchanged; retain actual instruction-work
observations instead of making a speed claim.

Keep the existing logical selector, constant inspector, expression arena,
continuation and zero/constant owners. No new classifier walk, numeric parser,
materializer, emission controller or VM representation is introduced.

### Preserve representation and behavioral boundaries

Native MSVC and strict GNU C++23 candidate measurements give SqlValue64,
materialized literal72, token24, payload80, properties16 and expression112
bytes. Adding the optional kind occupies existing literal padding and retains
the72-byte candidate. Verify the actual production layouts after the edit;
the candidate measurement is not implementation qualification.

Keep ordinary successful materialization and optimizer value consumers.
Do not change literal equivalence, grouping/ordering proofs, negative integer
generation, callback evaluation count, constant initialization, or diagnostic
priority in this foundation. No new source reparse occurs in optimizer or
lowering.

The full CodeTarget/CodeTemp owner still must distinguish early VALUES,
complete ordinary names, width checks, reached-once functions, deferred
constants and error accumulation. Preserving original INTEGER text does not
authorize a negative-hex-only patch or an unconditional first/last-error rule.
The signed-integer alias mismatch and the remaining16 preparation observations
stay explicit until their actual generation owner is implemented.

## Test-first, ownership and delivery

Begin with a focused binder contract test distinguishing INTEGER-token origin
from REAL storage for an overflowing decimal and a hexadecimal token from the
folded signed minimum. Record the genuine missing-field failure before
implementation.

Cover REAL spelling/underscores, aliases and generated/boolean controls,
caller/tree retirement and bound-owner moves. A valid parenthesized expression
must preserve the inner bound token range and original outer result name.
Existing malformed kind/token/range rejection remains required. The initial
wider-literal fixture was correctly rejected by `SyntaxTree::Create` before
binding; preserve that failed observation and correct the fixture/premise,
never weaken the existing invariant.

Retain source215's65 and23 semantic populations with no new mismatch and
unchanged diagnostic winners; do not call the whole populations compatible.
Record the expected general-path work for the folded signed-minimum control.
Run the complete fast tier, existing literal/default/alias/stack and
allocation-prefix owners, actual production layout checks and the selected
compiler/sanitizer requirements under original bounds.

Review this design before production edits. Each source retains focused
red/green, complete fast evidence, independent implementation acceptance and
normal commit/ledger traceability under ADR-0106. The actual change-aware
closing plan remains explicit and runs against the final exact batch head;
source acceptance does not waive its full-profile or hosted requirements.
Keep draft PR #293 within the related-slice bound. General generation,
factoring, IN, node43 and later DAG nodes remain incomplete.
