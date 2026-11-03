# ADR-0198: Native Literal Default Extraction

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #288 main `5ab82339`, ledger194/195
- Related contracts: ADR-0030/0036, ADR-0048, ADR-0106/0111 and ADR-0196

## Completed source mapping

ADR-0196 separated generated omitted INSERT defaults from the immutable
values used to read older short records. Its source and corrective
diagnostic descendant are delivered. The catalog producer can now be
corrected without changing inserted values through a shared pointer.
General generation/factoring, CREATE admission and remaining unary boundaries
still block IN query RHS.

The pinned SQLite extraction mechanism is distinct from expression generation:

| Responsibility | Pinned source |
|---|---|
| Retain the schema expression | `build.c:1779-1830` |
| Recognize unquoted Boolean identifiers during DEFAULT validation | `expr.c:2352-2362,2593-2597` |
| Normalize numeric underscores and transparent syntax | `parse.y:1182,1458-1469,2147-2149` |
| Extract a literal with affinity, including recursive minus | `vdbemem.c:1795-1999` |
| Preserve numeric/NULL values while numerifying text/blob | `vdbemem.c:893-916` |
| Prefer integer representation inside numeric/REAL affinity | `vdbe.c:354-428,455-461` |
| Admit an ALTER default using BLOB-affinity extraction | `alter.c:408-424` |
| Supply a missing-column value and final REAL affinity | `update.c:61-98` |

`valueFromExpr` starts exactly representable integer tokens as integers.
Otherwise it retains normalized, optionally signed token text before applying
affinity. BLOB affinity explicitly converts a FLOAT token to REAL and applies
NUMERIC affinity to an INTEGER token. General unary minus recursively extracts
its child with affinity, numerifies it, performs checked negation and reapplies
affinity. NULL remains NULL. Its temporary value is freed on allocation failure;
the successful caller owns the extracted value.

The completed current-source research has 52 paired short-record controls,
including eight STRICT ANY cases, with 31 differences. Examples include
TEXT `5.00`, negative zero and decimal-boundary spelling, wrapped INT64_MIN,
wide hexadecimal tokens and unary text/blob/NULL literals. Every input has a
native-created row that genuinely predates ALTER ADD COLUMN. An additional
18 recursive-affinity controls have 12 differences; these populations are
research observations, not unique-defect counts or TDD/qualification evidence.

Modern already has the required value policies in `ApplyAffinity`,
`CastValue(Numeric)` and `ParseSqliteReal`. The catalog loader owns literal,
blob and token decoding; the catalog model owns ordinary/STRICT affinity and
immutable final publication. STRICT ANY has BLOB affinity, unlike ordinary ANY.
The binder's existing `UnaryNumericLiteral` peek recognizes transparent
parentheses and positive unary wrappers over the same immutable syntax tree.

## Decision

### Preserve one extraction and affinity owner

Parameterize the existing catalog literal extractor with the effective column
affinity. Share the existing normal/STRICT classification through a small
catalog-internal entry point used by both the loader and model. Keep the
ordinary classifier and strict mapping canonical; add no public API or
second mapping.

An invalid STRICT type selects neither a fallback affinity nor a default
value. Retain its expression and leave rejection to the existing model
validation before publication. Keep the typed persistent-schema corruption
boundary; do not turn invalid metadata into a successful snapshot.

Retain the existing integer decoder and underscore normalization. Exact
integer values stay on the integer path. Decimal overflow and valid oversized
hexadecimal tokens retain their normalized text instead of becoming an
already-generated REAL value or falsely corrupting a valid schema. Preserve
malformed-token rejection. A direct negative decimal/real literal retains its
signed spelling whenever positive-magnitude integer classification cannot
represent it. Hexadecimal negation follows recursive extraction, not this
direct signed-token path.

Apply native numeric affinity during REAL-column extraction, not the public
forced-REAL publication policy. Native numeric affinity prefers an integer
inside recursion even for a REAL column. Premature conversion to REAL changes
the sign of zero for repeated minus and negated text/blob zero. Use the existing
NUMERIC policy for this phase, then retain `CatalogSnapshot`'s existing final
REAL publication. No alternative numeric parser or `SqlValue` representation
is introduced.

Recognize unquoted single-part TRUE/FALSE in the existing DEFAULT validator
before its ordinary column-reference rejection. Native constant-default
validation converts these identifiers before any table-column lookup, even
when the table has columns named TRUE or FALSE. Modern's bare DEFAULT parser
already creates Boolean literals, but parenthesized/unary forms retain
identifiers. The first 70-control qualification reached 64 matching pairs
before exposing this validation prerequisite at `-FALSE`; it was not a
completed qualification.

Factor the existing catalog Boolean-identifier classifier once and reuse it
for DEFAULT validation and extraction. Keep it allocation-free and retain the
immutable AST. Quoted and qualified identifiers, and ordinary column
references, remain rejected. CHECK/index name lookup and CREATE/INSERT
generation remain unchanged. A function default containing a Boolean argument
retains unsupported extraction metadata rather than invoking the function.
Ten native controls confirm Boolean defaults despite TRUE/FALSE column
shadows, quoted text and five rejected name/quoting forms.

Move the existing `UnaryNumericLiteral` peek into one syntax-internal AST
helper and reuse it in both binder callers and catalog extraction. Preserve
its algorithm and borrowed tree/literal lifetime. Do not introduce a
catalog-to-binder dependency or add an AST dependency to lexical token helpers.
Stop at nontransparent operators.

Unary plus preserves every supported literal value. General minus extracts
under affinity, uses `CastValue(Numeric)`, preserves NULL, promotes INT64_MIN
before negation and reapplies affinity. Reuse the current traversal and blob
decoder; owned bytes remain `ByteBuffer`. Keep checked arithmetic and the
existing typed allocation-failure boundary.

### Preserve publication and generation boundaries

`CatalogColumn::missing_record_value` remains the immutable, affinity-applied
value for a physically absent field. A pointer to SQL NULL is supported;
a null pointer with retained syntax is still the explicit unsupported-default
marker. Stored NULL is not a missing field.

Retain the same schema tree, expression IDs, shared ownership and final
snapshot validation. Add no parser replay, AST mutation, value cache, schema
field, callback invocation, evaluator or higher-layer dependency. Functions,
binary operations, COLLATE, CAST and other forms outside the current literal
subset remain unsupported markers rather than success-shaped NULL values.

Generated defaults remain independently owned by the selected INSERT
materializer. Extraction's numeric coercion must not replace generated
arithmetic: for example, BLOB extraction of `-'5.0'` is INTEGER -5 while native
generation is REAL -5. A readable wide-hexadecimal default can still produce
a typed value error when INSERT actually selects it.

More extractable literals can pass the existing mutation-target metadata
guard. Cover explicitly supplied INSERT, UPDATE and DELETE that need no
generated default, and preserve selected-default errors and no-mutation
behavior. Do not read an extraction value as a fallback for unsupported
negative text/blob/NULL generation. Their broader generation and CREATE
admission remain separate prerequisites; this source does not claim their
closure.

No persistent encoding, transaction transition, public session API, general
constant-generation architecture, benchmark family or later SQL feature is
added.

## Test-first, ownership and delivery

Add a focused loader regression before production changes and record its
observable failure. Use pinned-native SQL/database fixtures with real short
records and independent expected values. Cover spelling, zero signs, wrapped
decimal and hexadecimal boundaries, STRICT ANY, unary literal polarity,
supported NULL versus unsupported markers and malformed STRICT metadata.
Verify retained values after their loading database is released.
Add a separate owning red for the Boolean-validation prerequisite, including
column shadows and a malformed quoted-Boolean schema control. Requalify all
70 original pairs on the corrected tree without dropping the failing case
or relabeling the earlier partial run.

Exercise the producer through existing read/write consumers, including
stored NULL, generated-versus-extracted values, supplied mutations and selected
errors. Retain SQLite-writes/Modern-reads and relevant Modern-writes/SQLite-reads
evidence. Reuse existing default-generation, malformed and unsupported tests.

Extend the owning loader allocation test using the shared allocation-fault
mechanism, retaining its original all-fail controls. Sweep complete measured
allocation prefixes on a bounded fixture and verify cleanup/recovery without
changing deadlines or omitting allocations. Record a matched pre-change
construction/allocation baseline; this correction makes no speed claim.

Run focused green, complete fast units and the actual change-aware closing
plan, retaining strict compilation, sanitizer and native gates. Independently
review the design before implementation and the exact implementation before
its normal commit and append-only ledger record. ADR-0106 batch, exact-head
delivery, actual-main qualification and exact-ref retirement rules remain.
Node43 and IN query RHS remain incomplete.
