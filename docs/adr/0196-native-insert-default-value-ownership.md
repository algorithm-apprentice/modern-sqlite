# ADR-0196: Native INSERT Default Value Ownership

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #286 main `c9fbdbe0`, ledger185/186
- Related contracts: ADR-0030/0036, ADR-0048, ADR-0106/0111 and ADR-0160

## Completed source mapping

The VALUES/source-metadata correction and its diagnostic follow-up are
delivered. Remaining ordinary generation, constant factoring, unary and
default findings still block IN query RHS. The default ownership seam is
an independently bounded prerequisite; correcting it does not require the
larger statement-generation owner to be invented at the same time.

Pinned SQLite retains three distinct roles:

| Role | Pinned source |
|---|---|
| Own the schema DEFAULT expression, not its execution value | `build.c:1779-1830` |
| Extract an affinity-sensitive value for an old short record | `vdbemem.c:1802-1999` |
| Attach that extracted value to an ordinary column read | `update.c:30-98` |
| Map explicit INSERT columns, retaining the first ordinary occurrence | `insert.c:1064-1105` |
| Generate defaults only for omitted ordinary INSERT columns | `insert.c:1347-1414` |
| Retire the temporary column map | `insert.c:1653` |
| Generate a selected default through the expression factoring owner | `expr.c:5895-5947,6092-6111` |

The completed 32-case phase/spelling inventory contains nine native
extraction-versus-insertion distinctions. For example, a TEXT column's
`DEFAULT (5.00)` supplies `"5.00"` to an older short record but generates
`"5.0"` for a new INSERT. Large negative decimal spelling also distinguishes
the two owners. Neither a final affinity pass nor cloning the extracted
value can reconstruct the generated value.

Eight additional native preparation controls confirm omitted-default
selection, explicit NULL, no generated defaults for UPDATE/DELETE, prior
ordinary name/value errors and the last of two selected literal errors.
These controls use the pinned oracle without executing candidate mutations.

Modern already retains the immutable schema tree and its `SchemaExpression`
in `CatalogSnapshot`. `CatalogColumn::missing_record_value` belongs only to
old-record extraction. Nevertheless, `ResolveMutationTarget` currently
copies that pointer into every mutation column's `default_value`.
Lowering then clones it into INSERT constants. This couples separate native
owners and would make a later extraction correction change inserted rows.

Reuse the existing binder `MaterializeSyntaxConstant`,
`MaterializeSyntaxConstantLeaf`, `MaterializeConstantDefault`, literal
validator/materializer, affinity conversion and bounded continuation stack.
The existing ordinary default subset needs no second evaluator, syntax tree,
parser invocation, value cache, catalog field or VM instruction.

## Decision

### Separate generated values from extracted values

Resolve mutation target metadata without assigning extracted values to
`BoundMutationColumn::default_value`. That existing field means an owned
value generated for an omitted ordinary INSERT column, not a catalog default
in every mutation context. UPDATE, DELETE, explicitly supplied INSERT
columns and rowid-alias storage columns leave it empty.

After INSERT's existing target/value binding and duplicate-effectiveness
resolution succeed, materialize its selected defaults from the corresponding
retained catalog schema expressions. `DEFAULT VALUES` selects all ordinary
columns; positional VALUES supplies all columns; an explicit list selects
only omitted ordinary columns. Keep explicit NULL, duplicate ordinary
targets, hidden-rowid aliases and INTEGER PRIMARY KEY storage behavior.
No new default replaces an explicitly supplied value.

Use one temporary supplied-column map for an explicit list, populated from
the already resolved target IDs. It corresponds to native `aTabColMap` and
does not copy expressions or values. Do not repeatedly search the entire
VALUES list for each table column or add a persistent second target map.
The existing duplicate-effectiveness owner remains authoritative. No map is
needed for positional VALUES or DEFAULT VALUES.

Parameterize the existing syntax-constant materializer with its borrowed
`const SyntaxTree&`. CREATE TABLE continues using its current statement
tree; INSERT uses the existing immutable catalog definition. Token slicing,
dequoting and literal validation must all use that same owner. Reuse their
current kernels through explicit owner-aware entry points, not temporary
member replacement or a duplicated default walker. No borrowed syntax/token
view escapes materialization.

Preserve the catalog loader's existing legacy-variable normalization:
`catalog_loader.cpp:432-440` replaces a schema DEFAULT variable with a NULL
literal while retaining its original variable token. The materializer's
explicit schema-source entry must recognize that normalized NULL, using
the existing exact-token lexer check for its retained variable token.
Do not pass it through statement-only NULL spelling validation, weaken
ordinary CREATE/statement validation, accept arbitrary malformed NULL
tokens, allocate a parameter slot or fall back to the extracted value.

Reuse the existing `UnaryNumericLiteral` peek for the default materializer's
decimal signed-minimum shortcut, parameterizing that helper by the same
borrowed tree. Pinned `parse.y:1182,1458-1469` removes parentheses and folds
a leading positive unary node; `expr.c:5372-5395` then reaches direct literal
generation. Actual paired controls require INTEGER INT64_MIN, and its exact
TEXT spelling after affinity, for both `-((9223372036854775808))` and
`-(+9223372036854775808)`. An immediate-child inspection would incorrectly
generate a REAL value when switching from a correct extracted value to schema
generation. Keep ordinary binding on that same peek without changing its
algorithm. Stop at other payloads, retain the existing multiple-negation
continuation and do not add another wrapper walk or a hexadecimal correction.
The signed-minimum shortcut still runs the existing exact literal validator
before publishing a value; numeric-magnitude recognition is not validation
of a caller-supplied catalog AST token.

Publish only the resulting owned `SqlValue` through the existing immutable
shared pointer and existing lowering constant path. Keep catalog snapshots
unchanged. Typed materialization failures propagate; a catalog-derived
failure uses the mutation target's valid statement span while retaining
the literal diagnostic, rather than misidentifying a schema offset as an
offset in the INSERT text. Allocation failure unwinds the same statement
owner without partial publication or retry.

Visit selected function-free literal defaults in table-column order only
after ordinary binding succeeds. Retain the last typed value-requirement
error while visiting the remaining selected defaults, as the native
initialization drain does. Structural, unsupported-shape and allocation
failures stop immediately. Use the existing column sequence, not a second
deferred queue. A failed statement publishes none of its generated values.

### Preserve the bounded admission contract

This source does not broaden the literal-default grammar or change catalog
extraction. Preserve the current unsupported-default target admission check
and CREATE TABLE validation. An unmaterialized/unsupported catalog default
does not become SQL NULL. Existing literal/default limits, error carriers,
ordinary expression binding and unary behavior outside that shared signed-minimum
shortcut remain unchanged.

This is not acceptance of current catalog extraction, CREATE TABLE's early
materialization, general deferred-constant diagnostics, required negative
hexadecimal boundaries or nonliteral default execution. Those remain explicit
follow-ups. In particular, do not admit oversized hexadecimal schema defaults
by changing only the loader while insertion still uses an extraction value.
Any broader default admission or phase repair requires its own accepted
source and focused red.

No persistent format, transaction transition, public session registration,
query execution mechanism or dependency edge changes. This source makes
the existing insertion-value owner independent before its producer changes.

## Test-first, ownership and delivery

Start with a genuine binder regression using a validated catalog containing
the native extracted TEXT `"5.00"` and its retained `DEFAULT (5.00)` syntax.
Binding INSERT DEFAULT VALUES must generate `"5.0"` independently while
leaving the catalog extraction value untouched. This is a real public
catalog/binder contract, not a fabricated constant accessor or a claim that
the unchanged loader already extracts the correct spelling.

Cover signed spelling, explicit-list omissions, positional/explicit NULL
values, repeated targets, rowid aliases, unsupported defaults and selected
literal errors, including the last-error and earlier-ordinary-error controls.
Cover selected catalog-normalized legacy-variable NULL defaults and an
explicitly supplied value, while retaining malformed ordinary AST rejection.
Retire caller source/tree references and move the bound
statement before inspecting the retained values. Reuse existing lowering,
session, default-stack and mutation tests for unchanged execution paths.
Add complete owning allocation-failure sweeps for the new schema-backed
materialization and supplied-column map without increasing their ceilings.

Record a matched fixed-work/allocation baseline before implementation.
Correct ownership is the objective; make no timing-improvement claim.
Run focused green, complete fast units and the actual change-aware closing
plan, retaining strict compilation, sanitizer, native contracts and original
deadlines. Review design before production edits and implementation before
a normal source commit and append-only ledger record.

Related accepted default-owner sources may share one reviewable ADR-0106
batch. Each source remains independently buildable, tested and reviewed
before the next begins. Final exact-head qualification/review, normal
squash, actual-main qualification and exact delivered-branch retirement
remain mandatory. Node43 and later feature work stay incomplete.
