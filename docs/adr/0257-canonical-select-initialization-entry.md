# ADR-0257: Canonical SELECT Initialization Entry

- Status: Accepted
- Date: 2026-11-13
- Module graph node: `implement-advanced-sql`
- Prerequisite: PR #331 main `9be30993`, accepted source ledger393
- Related contracts: ADR-0005/0106/0111/0192/0254/0255/0256

## Confirmed entry discrepancy

Before selecting the remaining private source-preparation representation,
reconcile its existing construction entry. `RunSelect` currently checks
static compound widths before `InitializeCommon`, except when a multirow
VALUES core is present. `BindQuery` repeats the same width check after
initialization. Nested queries and mutation entry points already initialize
through the common owner first.

Consequently, a root `SELECT ?0 UNION ALL SELECT 1,2` reports compound width,
whereas its nested form reports the invalid parameter. Pinned SQLite reports
the parameter error in both forms. The retained expansion-entry investigation
already identified this discrepancy; a fresh current-source observation
reproduces it before implementation.

The original red/green observations used PR #330 main `9a648c6d`. Its
verification runner then rejected a child's report output as changed input.
PR #331 corrects that separate tooling prerequisite without changing SQL.
Requalify this slice on its merged baseline; neither earlier failed parent
report is closing evidence.

The same redundant gate hides errors from existing construction
normalization when that normalization is selected, such as a function
argument-limit error in a function-bearing AND expression. It must not be
mistaken for a source-expansion error.

## Selected source and owner map

| Responsibility | Existing owner |
|---|---|
| Native variable construction and admission | Pinned `sqlite3.c:185163-185170`, `sqlite3ExprAssignVarNumber` at114127-114204 |
| Native function construction limit | `sqlite3ExprFunction`, `sqlite3.c:113980-114008` |
| Native SELECT entry after construction | Parser SELECT reduction at184680-184690; `sqlite3Select` at157149-157217 |
| Native expansion before name resolution | `sqlite3SelectPrep`, `sqlite3.c:155992-156010` |
| Native later per-core width validation | `resolveSelectStep`, `sqlite3.c:112543-112548` |
| Modern input-reference validity | `ValidateTree` before semantic initialization |
| Modern shared initialization | `InitializeCommon`, `AssignParameters`, `NormalizeExpressionSyntax`, `NormalizeValuesConstruction` |
| Modern query admission | `BindQueryWithSourceTypeInfo`, `BindQuery`, `ValidateSyntacticCompoundWidths` |
| Shared SELECT facades | `BindSelectStatement`; `BindStatement` delegates SELECT to `RunSelect` |
| Typed failure and ownership retirement | Existing `BindError`, schema-check provenance and binder-owned arenas |

The reference remains the locally pinned SQLite 3.54.0 amalgamation. The
canonical corpus oracle caps variables at256 while the default Modern
binding option is32766. Do not compare those different upper boundaries as
equal profiles or claim raw diagnostic equality. `?0` is invalid under
both; configured-limit unit controls exercise the requested Modern boundary.

## Decision

Remove only the duplicate static-width preflight in `RunSelect`. Retain
statement-kind and AST-reference validation, then call `InitializeCommon`
exactly once before entering the existing common query path.

Keep the width check in `BindQuery`, actual bound-width validation,
parameter assignment, constructor-normalization eligibility and multirow
VALUES scheduling unchanged. No check is deleted from the query path,
reimplemented or moved into a second traversal. No source lookup, public
query, column storage, expression placeholder or ownership type is added.

Valid initialization followed by an unequal-width compound must still fail
with the existing compound-width diagnostic. Invalid parameters must fail
before that diagnostic through both SELECT binding facades. Existing
construction errors selected by the common initializer likewise retain
their owner rather than being hidden by a root-only shortcut.

The valid-query allocation/lifetime path is unchanged. Rejected compounds
now perform the same common initialization as other query entries; preserve
typed allocation failure and cleanup rather than promising unchanged work
for malformed inputs. No runtime optimization claim is made.

## Explicitly open boundaries

This is not complete native preparation ordering. The syntactic width gate
inside `BindQuery` still precedes source/name work, unlike the complete
native phases. Function-only expressions that do not select construction
normalization retain their separate limit-ordering issue. General ordering
among competing constructor diagnostics is not repaired here.

Private prepared-source ownership, soft expansion diagnostics, compound
conversion, per-core name/LIMIT/width ordering and `NC_NoSelect` remain open.
This correction admits neither IN nor a later graph node.

## Proof and delivery

Add focused regressions before production edits. Cover invalid low/high
parameter slots against compound width, both SELECT binding facades, both
operand positions, configured limits, a construction-normalization control,
and valid-parameter/valid-compound controls. Check that these non-schema
failures do not acquire a schema-check request.

Use the existing pinned oracle and distinguish error ownership from raw
diagnostic equality. Retain the function-only and source-width observations
as open boundaries, not passing repairs.

Run the focused owning tests, complete fast tier and existing binder
allocation/OOM controls. Apply the actual change-aware closing plan and
required hosted gates. Independent design acceptance precedes production
edits; implementation review and node/ADR evidence precede commit and delivery.
