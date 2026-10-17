# ADR-0100: Canonical Function Numeric-Type Conversion

- Status: Accepted
- Date: 2026-10-17

## Context

Reference-audit R1 reproduces two directions of aggregate numeric TEXT
misclassification. TEXT `1`, NUL, `e2` produces native INTEGER SUM `1` but
Modern REAL `1.0`. TEXT `9007199254740992`, NUL, `x` produces native REAL
but Modern INTEGER. A simple first-NUL truncation fixes neither the complete
classification contract nor the resulting integer-overflow latch behavior.

The main-agent follow-up uses the unchanged core fixture, pinned profile,
read-trace protocol, limits, and exact merged PR #209 tree. Of 168 paired
instances, 26 differ: 20 of 91 TEXT cases and six of 36 overflow/lifecycle
cases. All 36 BLOB and five existing scalar controls match. These are
overlapping observable instances of one conversion defect, not 26 bugs.
An additional SQL-only `sum(lower(X'...'))` probe reaches both directions
without a parameter or a new scalar function.

The existing native harness requires valid UTF-8 TEXT and nonempty BIND
payloads. The follow-up respects that boundary and covers empty values as
ordinary SQL literals; it does not change the harness or its limits.

Pinned SQLite 3.54.0 defines the mechanism in:

- `src/func.c:1944-1975`: SUM/TOTAL/AVG share `sumStep`, whose type decision
  comes from `sqlite3_value_numeric_type`;
- `src/vdbe.c:329-365,436-448`: numeric type uses
  `applyNumericAffinity(..., bTryForInt=0)` and `alsoAnInt`;
- `src/vdbemem.c:735-749,868-873`: UTF-8 real parsing stops at the first
  NUL, and exact integer round-trip uses `[-2^51, 2^51)`;
- `src/util.c:840-990`: decimal validity and real-syntax flags come from
  that zero-terminated parse; and
- `src/util.c:1156-1260`: integer fallback succeeds only when the complete
  original byte length contains an in-range integer and trailing spaces.

Modern's canonical mechanisms already include:

- one owned `SqlValue` and borrowed text/blob views;
- `BeforeFirstNull`, `ScanDecimalPrefix`, `IsCompleteDecimal`, and the
  pinned integer-algorithm REAL parser;
- one full-length `ParseIntegerPrefix` with checked range and trailing
  SQLite-space validation;
- `TruncateRealToInteger`, `RealSameAsInteger`, and `AffinityInteger`;
- one shared numeric TEXT affinity helper with complete-integer fast paths;
- distinct affinity, forced CAST, arithmetic, bitwise, and truth APIs; and
- one numeric aggregate state with checked addition, compensated summation,
  integer splitting, overflow latching, typed errors, and reset cleanup.

The aggregate-specific `find_first_of(".eE")` plus ordinary numeric affinity
duplicates the classification policy incorrectly. Ordinary numeric affinity
may integerize an integral REAL; function numeric typing must not do so.
The existing arithmetic and forced-cast policies are also not substitutes.

Begin after PR #209, main commit
`b692920e590e2f9eebd25dff7e775eb379fb84c4`, accepted tree
`439285f23cf7c8030726bf692e64717c0e94e678`. All nine first-head checks
passed, including the original unchanged sanitizer model in 4.58 seconds
under its ten-second deadline. Node 43 remains active.

## Decision

### 1. Publish one canonical function numeric-type operation

Add `ApplyNumericType(SqlValue value)` beside `ApplyAffinity` and `CastValue`
in the shared SQL-value module. It consumes the same move-only owned value
and returns either that unchanged value or an existing numeric alternative.
It allocates no new representation, owner, state, parser, or conversion
container.

Existing INTEGER, REAL, NULL, and BLOB remain unchanged. For TEXT:

1. Determine complete decimal validity and real syntax from the existing
   first-NUL view and scanner.
2. Leave TEXT unchanged if that decimal view is not complete.
3. Keep decimal-point or valid-exponent syntax REAL, even for integral values.
4. For integer syntax, choose INTEGER only when the parsed REAL round-trips
   exactly in `[-2^51, 2^51)`, or the existing full-length integer parse
   succeeds without overflow or non-space trailing bytes.
5. Otherwise return the parsed REAL.

Reuse the existing complete-integer fast path and the shared numeric TEXT
affinity helper. Give that helper an explicit function-numeric-type mode,
distinct from its existing numeric/integer-affinity and materialized-REAL
modes. Reuse the current scanner, full-length parser, REAL conversion, and
exactness predicates; do not implement a second decimal classifier in the
function registry.

Do not integerize an existing REAL or a decimal/exponent TEXT merely because
its value is integral. A NUL invalidates the full-length integer fallback
even when its preceding decimal view is complete. The lower 51-bit endpoint
is admitted and the upper endpoint is not.

### 2. Route numeric aggregates through that operation

Replace `NumericAggregateValue`'s local syntax search and ordinary affinity
with the canonical numeric-type operation on its existing explicit clone.
If TEXT or BLOB remains, use the existing forced REAL conversion, preserving
numeric-prefix fallback and REAL `0.0` for nonnumeric input.

Keep the current ownership, lazy state allocation, and argument-clone
boundary. Preserve the existing exact two-allocation numeric OOM control;
this is not an unrelated no-copy optimization.

Do not modify numeric state, count ordering, integer overflow detection,
the integer split threshold, compensated summation, error latching,
finalization, or reset. The correct INTEGER/REAL decision must naturally
choose those existing paths:

- a small integer-form NUL TEXT must retain an existing SUM overflow latch;
- a noninteger numeric type must clear that latch as before; and
- TOTAL and AVG retain their existing REAL finalization and overflow policy.

### 3. Keep distinct conversions and all consumers consistent

Ordinary affinity, forced CAST, arithmetic, bitwise, and truth behavior remain
unchanged. Preserve input TEXT/BLOB bytes and ownership when numeric typing
leaves them unchanged. Add no storage-affinity or IntReal representation
change; reference-audit R2 remains separate.

All global, grouped, DISTINCT, FILTER, local-order, compound, and private-query
consumers reach the existing aggregate descriptor and state. Correct them
there, not through a VM special case, rewritten SQL, extra callback, or
consumer-specific numeric conversion.

Production scope is the SQL-value contract/implementation and the canonical
function-registry conversion. Parser, binder, optimizer, lowering, bytecode,
VM, storage, public session, and later-node behavior are not changed.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before production changes. Add and observe
a focused existing-API aggregate-state red for both NUL directions and
overflow-latch persistence/clearing. Add shared-value and public-session
controls for types, exact values/REAL bits, ownership, and reset/rebinding.

Cover both 51-bit endpoints and adjacent values, full-length large integers,
int64 endpoints and overflow, decimal/exponent syntax before/after NUL,
whitespace, incomplete numeric prefixes, empty/nonnumeric input, BLOB,
existing REAL/signed zero/infinity, and long owned TEXT. Keep arithmetic,
forced-cast, affinity, and ordinary scalar controls explicit.

Extend the existing aggregate allocation helper rather than adding another
failure injector. Retain all old cases and exact allocation counts, exercise
long NUL TEXT classification paths, and prove typed failure plus cleanup and
successful reuse.

Replay the exact 168-case baseline with unchanged native outcomes. Add one
bounded numeric-type shard through the existing native harness, including
ordinary prepare errors and a native-valid unsupported control. Reuse the
unchanged core fixture and retain every old corpus/oracle/profile byte and
the 512-case cap. Use the already admitted lower/BLOB-to-TEXT path for a raw
fuzz seed, without changing the grammar generator or adding SQL syntax.

Replay all 2,511 frozen cross-feature instances against the post-A2 results
without new or changed-unresolved outcomes. Run focused and complete fast
tiers, all required compiler/sanitizer/static gates, the unchanged 68 native
contracts and 27 work properties, and a bounded grammar campaign. The matched
pre-change performance baseline is PR #209's unchanged native contract tier;
make no speed claim or budget/deadline change.

Require GPT-5.6 Sol implementation and exact-tree review, unchanged-input
qualification, and all nine exact-new-head hosted checks before normal
squash merge. Verify merged main before cleaning only the merged branch.
Other audit findings, ADR-0092 behavior deliveries, and node 43 remain open.

## References

- ADR-0015: SQL Values, Affinity, and Comparison
- ADR-0017: Scalar Function Registry
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0059: SQLite-Aligned Aggregate Function State
- ADR-0063: SQLite-Aligned Global Aggregate Execution
- [Implemented-engine reference audit](../sqlite-reference-audit.md), R1
