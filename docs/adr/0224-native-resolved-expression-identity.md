# ADR-0224: Native Resolved Expression Identity

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #301 main `ba66c197`, ledger262
- Related contracts: ADR-0033/0054/0056/0060, ADR-0106/0111,
  ADR-0159/0192 and ADR-0215/0222

## Confirmed failure and source mapping

The remaining generation investigation requires native expression identity,
not equality of already materialized SQL values. The existing binder
comparison also exposes an independently reproducible correctness failure:

```sql
SELECT 7.0 AS a,2 AS b
UNION ALL SELECT 3,7.00
ORDER BY 7.00;
```

Pinned SQLite matches the second column of the later core. Modern matches
the first core's numerically equal `7.0` and sorts by the wrong column.
The simpler `SELECT 7.0 UNION ALL SELECT 8.0 ORDER BY 7.00` is a native
preparation error but is currently accepted by Modern.

Parent-owned read-only controls on qualified main262 retain55 paired
observations: the initial9,25 literal/collation/search controls,15
normalization controls and6 scope controls. They authenticate the unchanged
pinned library, fixture and bounded trace executable. They are research,
not a behavioral red or a new build.

The controls distinguish REAL/exponent spelling, large integer spelling,
Boolean spelling and Boolean-versus-integer nodes, BLOB token spelling,
parameter spelling, special-function names, scalar DISTINCT and likelihood
probability tokens. They also retain successful small-integer normalization,
numeric underscores, parentheses, parser-folded constants, singleton-IN
rewrites, resolved Boolean-named columns and DQS controls.
Two controls expose wrong first-match output-column selection.
Root projection COLLATE currently causes false rejection as well.
Existing differences in diagnostic wording remain separately visible in the
raw transcripts; matching an error category is not a full-protocol claim.

| Responsibility | Pinned or current source |
| --- | --- |
| Native exact/differing-root-COLLATE/different comparison | `sqlite3.c:119465-119553` |
| Native expression-list child and ordering comparison | `sqlite3.c:119570-119579` |
| Compound candidate name binding and first matching column | `sqlite3.c:111990-112030` |
| Left-to-right core search and ORDER wrapper preservation | `sqlite3.c:112075-112170` |
| Ordinary ORDER/GROUP exact identity and last match | `sqlite3.c:112264-112310` |
| BINARY group-term identity for HAVING movement | `sqlite3.c:115635-115657` |
| Aggregate invocation and local-order argument identity | `sqlite3.c:120375-120435` |
| Numeric separator normalization and inline32 identity | `sqlite3.c:37150-37186` |
| Retained Boolean keyword token | `sqlite3.c:115149-115159` |
| Existing syntax and bound continuation comparers | `bound_select.cpp:6360-6798` |
| Compound, ordinary and grouping consumers | `bound_select.cpp:4545-4635,4720-4740,6825-6868` |
| Native construction, cached roots and normalization | `SyntaxNormalization`, `PrepareConstructionValuesRow`, `BeginBinding` |
| Scoped expression binding and arena retirement | `BindExpression`, `ActivateBindingExpression`, `AppendExpression`, `RestoreBindingArena` |

`BoundSemanticallyEquivalent` compares materialized literals by SQL value
and parameters by slot. Lowered special forms and the signed-minimum
literal can also erase distinctions that native comparison still uses.
The separate syntax prefilter used by ordinary ORDER is not a replacement:
raw syntax differs after accepted parser rewrites, and Boolean-looking
tokens may resolve to columns. Rebinding children, reparsing SQL, or adding
another AST walk for every comparison would duplicate existing ownership
and can change preparation errors or work bounds.

## Decision

### One resolved comparison owner

Use the existing iterative bound-expression comparison and continuation
stack as the canonical owner. Distinguish exact identity, a difference
confined to a root COLLATE wrapper, and a different expression, matching the
native comparison contract. A nonexact child is not an exact parent.

Retain the existing resolved source/rowid identities, alias targets,
function identities, aggregate ownership and ordered argument traversal.
Compare source columns before interpreting Boolean-looking syntax.
Subquery identity is not proven merely because a bound query ID matches;
retain native conservative rejection where its comparison rejects a
SELECT-bearing expression.

Native identity includes:

- equal inline signed32 integer nodes, including admitted normalized
  small-integer spellings;
- exact normalized token identity for non-inline integer and REAL nodes,
  without replacing their spelling by a parsed numeric value;
- decoded string identity but original BLOB token identity;
- Boolean native-node identity and token spelling, including the canonical
  token of a parser-generated Boolean;
- both assigned parameter identity and retained variable spelling;
- original case-insensitive function names, native DISTINCT attachments
  and retained probability-token identity for special forms; and
- the native unary/operand shape of the signed-minimum construction,
  rather than equality with an unrelated materialized integer minimum.

Reuse the existing integer classifier, literal/token validation, dequoting,
name comparison and construction helpers. Numeric separator normalization
must have one shared mechanism for materialization and comparison; do not
add a second owning normalized-token cache. Comparing identity must not
evaluate expressions, invoke functions, generate bytecode or introduce an
earlier value-demand error.

### Binder-local origin ownership

Retain one private, expression-indexed origin table in `StatementBinder`.
Entries borrow typed syntax IDs from its already owned `SyntaxTree`; an
explicit absent origin denotes an existing synthesized bound node.
This is a provenance link, not a second expression tree, value cache,
syntax owner or generation-site registry.

Associate origins at the existing root and continuation binding boundaries.
Preserve an original bound node's origin through actual internal
copies and alias targets. A cached constructor binding keeps its original
origin rather than acquiring the caller's syntax.
Use the bound payload and existing construction metadata to identify
intentional synthetic nodes; do not interpret an arbitrary missing or
stale index as successful equivalence.

The origin table and bound-expression arena have one shared lifetime and
index invariant. Append and rewind them coherently, including speculative
compound candidates, discarded ORDER trials, aggregate copies, typed
failures and allocation unwinding. The ordinary binder exception boundary
still owns allocation failure. No partially published statement or
success-shaped fallback is permitted.

The table has one entry per actual bound node and shares the existing
expression-arena capacity calculation in `ReserveOutputStorage`, rather
than independently growing or predicting a second arena. It is not a
per-query copy or a reservation of configured maximum capacity.
Do not retain it or the AST after binding,
export syntax IDs into public plans, enlarge public expression carriers, or
introduce an owning raw pointer.

### Reuse the contract at native identity consumers

Route the current native expression-identity consumers through this one
comparison owner. Exact-match consumers remain exact: ordinary ORDER/GROUP
result reuse, aggregate invocation reuse and aggregate order-key reuse.
Compound result matching and BINARY group-term checks accept only the
native exact/root-COLLATE outcomes appropriate to their existing callers.
Do not silently conflate either with runtime SQL comparison.

Keep alias and ordinal precedence, the first matching compound core/column,
the last exact ordinary ORDER/GROUP result match, explicit ordering
collation, wildcard source identity and existing clause/error order.
The compound ORDER term's existing wrapper stripping and final collation
publication remain separate from expression identity.
Do not discard nested COLLATE differences.

Remove or reuse the superseded ORDER syntax-prefilter logic rather than
maintaining a second competing expression-comparison traversal.
Do not change runtime collation, sorting, membership storage, aggregate
functions, SQL-value equality or parser normalization.

This corrects the binder's existing identity roles. It does not activate
deferred/reached-once generation, expose the private origin table to a
lowerer cache, assign global eager creation IDs, repair every remaining
signed-generation diagnostic, or admit IN query RHS or a later DAG node.

## Test-first, boundedness and delivery

First add one focused owning binder regression for the wrong first-match
result-column choice and record the actual unchanged-production failure.
Then cover the confirmed rejection/acceptance pairs, right-core/column
precedence, root versus nested COLLATE, normalized and signed forms,
special functions, parameters, Boolean-named columns and DQS.
Include a session-level row-order control for the demonstrated query.

Exercise the shared consumers with focused aggregate reuse and ordinary
ORDER/GROUP controls so stricter identity does not merely move the bug.
Retain current speculative-error/schema invalidation and alias/constructor
tests. Extend existing allocation-failure coverage for the new origin
lifetime without increasing its ceilings or introducing a later testing
phase.

Record a matched binder allocation/footprint baseline before implementation.
Keep traversal iterative and account for actual private storage and work;
do not claim a speed improvement or add a new benchmark family.
Public contracts/layouts and all existing limits remain unchanged.

Independent design acceptance precedes production changes. Focused
red/green, the complete fast-unit tier, the actual change-aware closing plan
and implementation/evidence review precede a coherent normal source commit.
Keep node/ADR traceability and the existing reviewed-slice delivery rules.
Final exact-head hosted/integrated/main qualification and guarded cleanup
remain mandatory. Do not mark the broader generation prerequisites, node43
or the project complete with this correction.
