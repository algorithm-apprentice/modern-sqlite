# ADR-0199: Native CREATE Default Syntax Ownership

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #289 main `41005bac`, ledger196-199
- Related contracts: ADR-0048, ADR-0106/0111, ADR-0189 and ADR-0196/0198

## Completed source and ownership mapping

ADR-0196 separates selected INSERT defaults from old-record extraction.
ADR-0198 corrects that extraction and its Boolean validation. Both are
delivered. CREATE still eagerly generates a default value even though its
lowering persists the retained SQL, not that value. Correct this declaration
owner before repairing the remaining generated-default and general
generation/factoring boundaries. Node43 and IN query RHS remain incomplete.

| Responsibility | Pinned SQLite source |
|---|---|
| Validate and retain a DEFAULT expression and its spelling | `build.c:1779-1830` |
| Replace and free a prior default for the same column | `build.c:683-704` |
| Recognize only unquoted Boolean identifiers before column lookup | `expr.c:2352-2362,2593-2597` |
| Distinguish declaration constants and legacy schema variables | `expr.c:2533-2665,2894-2916` |
| Generate a selected omitted INSERT default later | `insert.c:1347-1414`; `expr.c:5895-5947,6092-6111` |
| Independently extract an old-record value | `vdbemem.c:1795-1999` |

`sqlite3AddDefaultValue` checks constant syntax and owns a compressed copy
with its source span. It does not generate an insertion value or apply column
affinity. Every reached clause is validated; `sqlite3ColumnSetExpr` replaces
the preceding expression only after that validation. The parser expression
and temporary span storage are retired through the existing allocation and
error paths.

Modern already owns the complete original statement text and canonical
schema SQL in `BoundCreateTable::Impl`. The latter retains original DEFAULT
spelling from the table-name-through-statement source. The public
`BoundCreateColumn::default_value` is an unnecessary generated-value owner;
CREATE lowering writes `canonical_sql` through `EmitSchemaRecord`.
Logical and physical CREATE planning consume no-op metadata, not defaults.
Catalog loading already validates each DEFAULT and retains the last
expression and extracted value.

Existing reusable mechanisms include exact literal-token validation,
`SourceSpan`/`Slice`, canonical catalog name comparison and the allocation-free
catalog `DefaultBooleanValue`. The selected INSERT materializer owns numeric
conversion, affinity, signed-minimum handling, the bounded negation stack and
legacy schema-variable NULL normalization.

The existing construction `IsParserTimeConstant` is not a declaration
validator: it requires construction-normalization storage, treats variables
as constant and resolves function determinism/arity. Preserve its Normal/NoIs
semantics rather than changing its contract for this literal-only admission.
No general constant evaluator or second value-generation walk is needed.

Fourteen paired CREATE observations confirm native admission of wide and
negative-wide hexadecimal and negative text/blob/NULL defaults that Modern
rejects during eager generation. Negative hexadecimal minimum is admitted
by both at CREATE; its later generation remains a separate prerequisite.
Three phase/cleanup controls show that parenthesized `"FALSE"` is a native
prepare-time not-constant error. Modern instead prepares and then reports
corruption during Step and Reset. The operation rolls back: autocommit stays
true, the closed file is empty and native schema count is zero. This is an
admission/error-phase defect, not evidence of persisted corruption.

Nine further native controls establish last-literal-default selection,
discarded versus selected wide hexadecimal errors, rejection of either
invalid quoted clause, bare quoted strings, Boolean column-name shadows,
existing-target no-op admission and duplicate-column error ordering.
An eight-case, four-repetition binder allocation baseline excludes parser
and catalog setup. These observations are research and baseline evidence,
not focused red/green or final qualification.

## Decision

### Retain declaration syntax in its existing owner

Replace `BoundCreateColumn::default_value` with
`std::optional<SourceSpan> default_expression`. A present span refers to the
same bound table's owned `source()`, never to `canonical_sql()` or a destroyed
syntax tree. Absence means no DEFAULT; an explicit DEFAULT NULL has a present
span. Preserve source/canonical SQL ownership and move semantics.

This supersedes only CREATE's eager materialization described in ADR-0196.
Do not retain a compatibility value pointer, cache an evaluated value, retain
another AST or reparse SQL. INSERT's independently generated value and
catalog's immutable missing-record value keep their distinct contracts.

Validate the currently supported literal-default shape without evaluation:
walk transparent parentheses and unary plus/minus, then validate the exact
literal token. Admit the existing NULL, numeric, text, blob and unquoted
Boolean leaves. Do not parse a numeric magnitude, compute a sign, decode
literal payload storage or apply affinity merely to declare a table.
The one-operand walk is iterative and requires no continuation allocation.

Move the existing Boolean-identifier classifier to one catalog-internal
helper. Use the same raw source token and `CatalogNamesEqual` for CREATE,
catalog validation/extraction and the schema-backed default leaf.
Only a single unquoted TRUE/FALSE part is Boolean; quoted and qualified
identifiers are not. Preserve the parser's separate bare string-token
DEFAULT representation, including bare `DEFAULT "FALSE"`.
Binder already depends on catalog; add no catalog-to-binder or
syntax-to-catalog edge.

Introduce typed `BindErrorCode::InvalidDefault` for invalid default
identifier/variable leaves, with the native not-constant diagnostic and the
declaration expression's statement-source span. Map it to the ordinary
generic database error. It is not a deferred value-requirement error.
Malformed caller-supplied literal tokens retain their existing typed
internal-input rejection; syntax-only admission is not permission to bypass
token validation.

Validate every reached literal DEFAULT clause in declaration order, then
retain its span. A later valid clause replaces the selected span; it does not
erase an earlier invalid declaration. Keep other column-constraint and
duplicate-column ordering. The original SQL retains every clause so the
existing catalog loader independently selects the same final expression.

### Preserve no-op, persistence and generation boundaries

Leave the existing-target IF NOT EXISTS return before executable default
admission. ADR-0189's read transaction access, non-read-only classification,
guard, cleanup and no-write behavior remain unchanged.

Keep the current TEMP/STRICT/WITHOUT ROWID and constraint-shape boundaries.
This correction does not admit function, current-time, binary, COLLATE,
query or other nonliteral defaults. Those forms retain explicit unsupported
errors rather than acquiring success-shaped NULL values or callback
execution during preparation.

The selected INSERT materializer remains the value-generation owner. Preserve
its selected-column ordering, exact numeric diagnostics, signed-minimum
shortcut, schema-origin token handling, bounded negation continuation and
owned result publication. Negative text/blob/NULL and required hexadecimal
generation corrections remain subsequent prerequisites; do not use catalog
extraction as their fallback.

The writable facade still detaches the execution context, reloads the
candidate catalog and only then completes schema success. Load failure
retains `FailAndRollback`. No persistent encoding, transaction transition,
session API, opcode, SQL feature or benchmark family is added.

## Owning TDD and delivery

Start with an actual binder failure requiring native-valid wide-hexadecimal
CREATE to bind without generating its default. Cover quoted/nonconstant
prepare errors, literal signs, Boolean shadows and quoting, last-default
selection, discarded value errors, duplicate-column ordering and no-op
behavior.

Inspect raw retained spans after caller strings and syntax owners are
released and the bound statement is moved. Distinguish absent defaults from
explicit NULL. Replace creation-time value assertions with the new metadata
contract, while retaining deep signed-generation assertions under the actual
schema/selected-INSERT owner. Keep malformed-token and unsupported-shape
coverage.

Extend the existing owning binder allocation-failure sweep without raising
its allocation ceiling or deadline. Verify recovery and publish no partial
bound owner. Compare matched before/after declaration allocations without
claiming a timing improvement.

Exercise real CREATE/INSERT/read consumers in both writer/reader directions,
including supplied versus selected defaults, repeated clauses, exact
persisted SQL, native integrity checks and preparation failures before
mutation. Keep source-specific research failures explicit; they are not
reclassified as successful evidence.

Run focused green, complete fast units and the actual change-aware closing
union, including strict compilation, sanitizer, native and quality owners.
Require independent design and exact-source review, a normal source commit
and append-only ledger before the next slice. Related default-owner
corrections may share an ADR-0106 batch with per-source evidence and final
exact-head gates. Normal squash, actual-main qualification and retirement
of exactly the delivered refs remain mandatory.
