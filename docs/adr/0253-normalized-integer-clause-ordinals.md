# ADR-0253: Normalized Integer Clause Ordinals

- Status: Accepted
- Date: 2026-11-11
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #326 main `9b9a3500`, ledger370
- Related contracts: ADR-0056/0106/0111/0161/0224/0251/0252

## Confirmed bounded classifier gap

The expansion/`NC_NoSelect` investigation exposed a separate consumer of
already canonical construction metadata. Native parser folding can replace
an expression with an integer node before ORDER or GROUP ordinal selection:

```sql
SELECT id FROM t ORDER BY 1 IS NOT NULL;
SELECT column1,count(*) FROM t GROUP BY 1 IS NOT NULL ORDER BY 1;
```

With rows `(7,40),(2,30),(9,20)`, pinned SQLite orders the first query as
`2,7,9` and returns three one-row groups from the second. Modern treats the
key as an ordinary constant, retaining insertion order and one three-row
group. `1 IS NULL` similarly becomes ordinal zero natively, but Modern can
accept it as an ordinary constant instead of reporting the range error.

Twenty-four paired statement observations establish seven wrong row/group
results, five native-invalid preparations accepted by Modern, two native-valid
compound preparations rejected by Modern, and two diagnostic-priority
differences. Eight complete protocol controls agree, including Boolean
constants, an inner COLLATE barrier, large integer tokens and ordinary
arithmetic. Both shared facades have the same observations.

Four cached-schema controls inspect metadata before stepping. A folded
zero before a later missing key incorrectly permits that later schema
request. The other three controls retain their actual requests. Existing
construction, lexical validation and parameter ownership are already correct
for this bounded population; the missing step is ordinal classification.

This is a small independent correction in the same clause-context audit.
It does not constitute the larger source-expansion or `NC_NoSelect` design.

## Completed selected native and canonical map

| Responsibility | Pinned or canonical owner |
| --- | --- |
| Literal null-test folding and unary transparency | `sqlite3PExprIsNull`, `sqlite3.c:180813-180849`; ADR-0161 |
| False-AND integer construction and operand retirement | `sqlite3ExprAnd`, `sqlite3.c:113955-113976` |
| Integer node identity | `sqlite3ExprInt32`, `sqlite3.c:113791-113804` |
| Integer/signed-operand classification | `sqlite3ExprIsInteger`, `sqlite3.c:115756-115803` |
| Ordinary ORDER/GROUP selection and alias precedence | `resolveOrderGroupBy`, `sqlite3.c:112253-112312` |
| Core-major compound ordinal selection | `resolveCompoundOrderBy`, `sqlite3.c:112057-112164`; ADR-0251 |
| Existing normalized integer/Boolean distinction | `ParserConstant`, `SyntaxNormalization`, `NormalizeExpressionConstruction` |
| Shared three-consumer classifier | `ClassifyOrderByInteger`, `OrderByOrdinal`, `BindGroupBy` |
| Existing output references and query-containing copies | `BindOrderByResultReference`, `BindDirectOrderingCopy`, `BindResultDefinitionCopy` |

`sqlite3ExprIsInteger` tests integer-node identity before following ordinary
unary signs. It does not evaluate arbitrary constant SQL. Native Boolean
nodes, including an empty-IN constructor result, are not integer ordinals.
A subsequent eligible AND fold can produce an integer zero instead.

Modern already records this distinction as `FalseInteger`/`TrueInteger`
versus `FalseBoolean`/`TrueBoolean`. Its ordinary binder and structural
comparison consume those tags, but the shared ordinal classifier currently
examines only wrappers and original integer tokens. Recomputing constructor
rules in that classifier would create a second mechanism.

## Decision

At each node visited by the existing shared integer-classification loop,
recognize an available canonical normalized integer tag as zero or one and
apply the loop's accumulated sign. Retain the existing no-annotation path.
Do not allocate another annotation, bind an expression, materialize a SQL
value, evaluate a function or reconstruct parser folding.

Keep the distinction from normalized Booleans explicit. Ordinary arithmetic,
NULL expressions, parameters and tokens outside the existing native small
integer domain do not become ordinals merely because their runtime value
could be an integer. Retain literal-token validation and the canonical
constructor's lexical-before-deletion guarantee for malformed public syntax.

Use the one shared classifier for ordinary ORDER, compound ORDER and GROUP.
Preserve parentheses, accumulated unary signs, root COLLATE handling and the
COLLATE barrier below a unary operator. Do not change alias precedence,
first/last expression-match rules or the compound core-major encounter order.

The existing consumers continue to perform range checks, reference the
selected output, copy real query-containing definitions where required, and
apply their existing collation, grouping and aggregate checks. No alternative
ordering key, placeholder value, query owner, runtime operation or public
carrier is introduced.

A normalized zero must stop at the same existing ordinal range boundary.
Do not reach a later missing name merely because the original syntax was not
an integer token. A real missing-name request reached before that range error
must survive. Do not infer or repair schema flags from the eventual error.

No construction rule, discarded-subtree lifetime, scalar-width policy,
source-expansion phase or comparison query-descending policy changes here.
The classifier remains bounded by the existing iterative wrapper walk and
creates no additional owning resource.

## Focused proof and delivery

After independent design acceptance, record a focused owning red for a
normalized integer failing to select the first result column. Cover ordinary
ORDER, compounds and GROUP through the shared owner, not only the observed
constant-AND spelling.

Assert actual small sorted/grouped results through the public shared paths.
Retain signed and nested-sign cases, literal null-test categories, root and
inner COLLATE boundaries, Boolean-versus-integer constructors, alias
precedence, large tokens, ordinary arithmetic and malformed syntax controls.
Keep exact range/aggregate errors and real schema-request ordering.

Exercise query-containing result references, owner moves, reset/rebind and
recovery after rejected preparation. Extend the existing binder allocation
prefix owner for newly reached reference/copy and range-error paths; retain
all prior cases, allocation ceilings and deadlines. Unchanged lowerer and
runtime mechanisms still receive their selected complete qualification.

Record a matched allocation baseline for already successful wrong-result
and unchanged controls. Newly admitted compound preparations use their
corresponding explicit-ordinal controls for resource shape; failed
preparation is not a performance baseline. This is correctness, not a speed
claim or a native-malloc comparison.

Run focused green, complete fast units, strict owning analysis and the actual
change-aware closing plan. Preserve independent source/integration review,
normal protected squash, separate actual-main qualification and expected-OID
source retirement. Keep the slice coherent and linked to node43 and this ADR.

The fifteen mapped source-expansion diagnostic differences, six extra and
three lost `NC_NoSelect` schema requests, later key-info/error arbitration,
singleton/local VALUES aggregation and remaining generation remain open.
No IN or later DAG node is admitted, and node43 is not complete.
