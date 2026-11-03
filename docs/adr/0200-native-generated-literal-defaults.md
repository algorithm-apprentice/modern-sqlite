# ADR-0200: Native Generated Literal Defaults and Shared Subtraction

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: ADR-0199 source200, `01ce6838`, ledger200, draft PR #290
- Related contracts: ADR-0015, ADR-0106/0111, ADR-0124/0140 and ADR-0160/0196/0198

## Completed source mapping and evidence

Source200 separates CREATE default admission from selected INSERT value
generation. Its independently accepted source is committed and published.
The remaining generated-literal correction can now reuse that declaration
owner without rejecting native-valid CREATE statements.

| Responsibility | Pinned SQLite source |
|---|---|
| Select omitted INSERT defaults | `insert.c:1347-1414` |
| Generate INTEGER with a separate negative flag | `expr.c:4426-4463` |
| Distinguish direct numeric minus from general zero-minus | `expr.c:5372-5395` |
| Normalize numeric underscores | `parse.y:2147-2150` |
| Classify arithmetic operands without affinity conversion | `vdbe.c:469-512` |
| NULL, checked integer, REAL and NaN subtraction | `vdbe.c:1951-2038` |

Direct negative INTEGER generation admits decimal INT64_MIN but rejects
hexadecimal INT64_MIN. Direct FLOAT generation applies the sign to the parsed
literal, preserving negative zero. A general minus instead generates integer
zero minus its child. Numeric affinity or NUMERIC casting is not arithmetic
coercion: text/blob `"5.0"` generates REAL, not an already coerced integer.

Modern already has one literal materializer, exact lexical validation,
`UnaryNumericLiteral`, a bounded default continuation, schema-variable NULL
normalization and final column affinity. `SqlValueAccess` owns borrowed
numeric operands, explicit conversion scratch and primitive publication.
The VM's binary subtraction already uses those policies and base
`CheckedSubtract`. Its result is computed before a possibly aliased register
is overwritten. `SetReal` preserves native NaN-to-NULL behavior.

Thirty current-source ordinary controls have eleven differences, and
twenty-four generated-default controls across four affinities have seventeen
phase/value differences. Examples include rejected text/blob/NULL defaults,
incorrect generated hexadecimal minimum and the sign of a nested BLOB zero.
The protocol retains exact REAL bits. Wider negative defaults fail in both
engines, but Modern's default error loses the minus. Three native diagnostic
controls also confirm removal of numeric underscores in error spelling.

Fourteen additional ordering/grouping controls establish an important boundary:
a normal negative hexadecimal minimum followed by a missing name must report
the name error, while the same literal in an eager VALUES constructor precedes
an outer missing name. A negative literal precedes a later scalar-width
generation error. Grouping positive and negative zero preserves the separate
result expression's bits.

An eight-case, four-repetition binding/default baseline excludes parser and
catalog setup. Its allocation counts are 11/11/14/16/23/34/12/10. Native MSVC
layouts remain bound expression112, payload80, materialized literal72,
token24 and unary8 bytes. Source200's complete green arithmetic, alias,
allocation and sanitizer owners provide the subtraction-refactor baseline.
These observations are not successor TDD or a timing-improvement claim.

## Decision

### Share the existing subtraction implementation

Move binary subtraction into one private
`SqlValueAccess::Subtract(output, left, right)` operation. Keep its current
algorithm: NULL before conversion, borrowed `NumericOperand` with explicit
scratch, base `CheckedSubtract`, REAL fallback and canonical primitive stores.
It accepts every SQL storage class through the existing arithmetic conversion
policy; it does not use affinity or NUMERIC casting.

The output may alias either input. Compute the scalar result before storing,
and consult no borrowed input or conversion scratch after publication.
Preserve reusable VM output backing and release external ownership through
the same setters. Do not construct a temporary owned result per VM instruction.
Allocation exceptions remain at the existing owning boundaries.

Keep this arithmetic body out of line in the production `sql_value.cpp`,
with only its private declaration in the header. Owning test translation
units intentionally do not inherit production sanitizer instrumentation.
An initial inline candidate emitted both instrumented and uninstrumented weak
definitions; the actual unit executable selected the uninstrumented body.
Retain the PRIVATE sanitizer policy and verify the final production definition
and linked body rather than treating compile options as instrumentation proof.

Route `BinaryOperation::kSubtract` through this helper and remove its old
arithmetic branches. Other binary operations and VM unary negation remain
unchanged. Keep invalid bytecode rejection in the existing verified dispatch.
Add no arithmetic-operation enum, public value representation, register,
allocator, cache, parser or new dependency direction.

### Generate selected numeric signs through the common kernel

Parameterize the existing private literal materializer for negative
INTEGER/FLOAT generation. Reuse existing underscore normalization, the
decimal-minimum classifier and `ParseSqliteReal`; add no numeric parser.
Keep exact lexical validation before a signed-minimum shortcut.

Generate direct decimal INT64_MIN as INTEGER, preserve direct FLOAT sign,
and reject negative hexadecimal minimum and oversized hexadecimal values.
The typed value diagnostic includes the sign and normalized numeric spelling.
Use the already normalized token for positive hexadecimal overflow messages
too; this corrects spelling only, without changing positive admission,
error phase or range policy.

The ordinary already-valid decimal-minimum shortcut can call this kernel
only after its existing recognition and validation. Do not activate new
ordinary negative errors or flatten ordinary signed expressions in this
source. Preserve current bound-expression and diagnostic footprints.

In the existing selected-default continuation, recognize a direct
INTEGER/FLOAT leaf through the shared parenthesis/positive-wrapper peek,
validate it, then materialize its native sign. Keep the existing bounded
continuation for enclosing general minus nodes; do not collapse them by
parity or optimize away their ownership/failure coverage.

Each general minus uses the shared subtraction helper with a named integer
zero input. Compute into a clean local result before moving it back.
Generated numeric values must not unnecessarily retain the consumed BLOB
literal's backing; VM registers retain their separate reusable-backing policy.
NULL stays NULL, text/blob arithmetic preserves REAL syntax, INT64_MIN
subtraction promotes safely and general zero-minus has native zero bits.
Apply column affinity only after generation, through the existing caller.

### Preserve the remaining owner boundaries

Keep source200 CREATE/no-op admission and SQL spelling, catalog missing-record
extraction, selected-column/error ordering, supplied/rowid-alias skipping,
source lifetime and legacy schema-variable NULL normalization. Functions,
binary expressions, current-time and other nonliteral defaults remain
explicitly unsupported. No parser, VM opcode, session API or persistent
transition changes.

Ordinary unary correction remains separate. Eagerly adding its missing
hexadecimal error would regress the currently matching ordinary missing-name
winner until the general name/generation owner is corrected. Switching VM
unary negation to zero-minus without first distinguishing its direct numeric
generation owner would regress correct direct negative zero.
This source therefore closes generated literal defaults and shares binary
subtraction, not ordinary unary, general CodeTarget/CodeTemp/factoring or IN.

## Test-first and delivery

Record a genuine selected-default binder red for native REAL -5 from
`-'5.0'`, independently of its old-record INTEGER extraction.
Cover text/blob/non-numeric/embedded-NUL/NULL, direct and repeated zero signs,
wrapped decimal and hexadecimal boundaries, signed diagnostics, final
affinity, omitted/supplied defaults and preceding ordinary errors.
Retain deep500 generation controls and complete allocation-prefix sweeps.

Runtime and VM owners cover subtraction, overflow, REAL, NULL, NaN, both
alias directions, borrowed input lifetime and retained output backing.
Keep all existing arithmetic, zero-allocation and resource-limit assertions.
Use real CREATE/INSERT/SELECT and both writer/reader directions without
confusing generated values with short-record extraction.

Retain the ordinary30 and ordering14 controls with their unclosed differences
explicit. Require no new mismatch among currently matching controls; do not
call those whole populations compatible.

Run focused green, complete fast units and applicable exact-source
native/sanitizer/optimized/static/OOM checks, matched allocation controls and
independent source review before a normal successor commit and ledger record.
The native55 and full hosted/manual union run on the final exact PR #290 batch
head. Preserve all assertions, workloads and deadlines. IN and later nodes
remain blocked by the other generation prerequisites.
