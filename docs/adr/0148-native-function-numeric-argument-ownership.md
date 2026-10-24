# ADR-0148: Native Function Numeric-Argument Ownership

- Status: Accepted
- Date: 2026-10-24
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 13

## Source map and executed baseline

PR #252 closes the confirmed shared-read/VM corrections. Functions remain
the next unresolved mechanism, not permission to start later SQL features.
The unchanged global-functions control is 5.351735x wall in nine ordinary
samples per engine. Equal-work ordinary profiles complete 1,257 iterations,
82,378,752 input items and 1,257 rows with identical digest and lifecycle.
Passive samples show numeric-kernel cloning, moves and conversion alongside
Column and typed-dispatch costs. This does not attribute the entire gap to
one helper or promise a speedup.

The VM already passes borrowed register spans. Inside the function registry,
`NumericAggregateValue` still clones every input and moves it through the
owned `ApplyNumericType` API. `abs` clones noninteger inputs before REAL
conversion; `sign` clones before ordinary affinity. These consumers need
numeric observations, not independent ownership of argument bytes.

| Responsibility | Pinned SQLite / canonical Modern owner |
|---|---|
| Borrow aggregate register arguments | `vdbe.c:7982-8123`; existing VM argument spans |
| Lazy state before numeric typing; checked sum, compensation and overflow | `func.c:1870-1975`; `AggregateState` and `NumericAggregateState` |
| TEXT-only function numeric typing and exact integer fallback | `vdbe.c:329-365,436-448`; ADR-0100's shared numeric TEXT helper |
| Borrow INTEGER/REAL or read a TEXT/BLOB REAL prefix | `vdbemem.c:715-785`, `vdbeapi.c:207-209`; existing SQL-value scanners |
| INTEGER/NULL versus REAL absolute value | `func.c:198-236`; existing `AbsFunction` |
| Numeric-type-only sign admission | `func.c:2640-2654`; existing `SignFunction` |
| Lazy context, finalization and release | `vdbeapi.c:1200-1225`, `func.c:2024-2074`, `vdbemem.c:506-534`; existing state/finalize/reset owners |

The local pinned source is authoritative. Keep sum count ordering, checked
integer addition, 51-bit conversion boundaries, large-integer splitting,
compensation, overflow latching and final results unchanged. Window inverse
state is deferred; no window API or new aggregate kind is introduced.

## Decision

### Share the existing numeric operand borrow

Generalize private `SqlValueAccess::ArithmeticNumeric` into one
`NumericOperand` operation with explicit `Arithmetic` and `Function`
policies. Both policies borrow existing NULL, INTEGER and REAL values.
Only conversion uses the caller's existing optional `SqlValue` scratch.
Rvalue inputs remain forbidden, and neither argument nor scratch may change
while its returned reference is in use.

Arithmetic retains its current coercion unchanged. Function policy uses the
same TEXT conversion helper and function-numeric-type mode as the public
owned `ApplyNumericType` operation. Converted TEXT yields the existing
primitive numeric alternative in scratch; unconverted TEXT and BLOB remain
borrowed, matching native numeric-type admission. Do not clone bytes to ask
whether they are numeric.

Replace the private arithmetic call sites directly. Do not retain a wrapper,
second operand selector, numeric representation, conversion parser, owner,
cache or generic callback framework.

### Read REAL values through the canonical conversion

Provide one private infallible numeric REAL observation beside the
existing typed SQL-value accessors. Existing INTEGER/REAL return their numeric
value, TEXT/BLOB use the existing first-NUL view, decimal-prefix scanner and
SQLite REAL parser, and NULL observes native REAL zero.

Share that REAL-prefix implementation with the existing REAL CAST and truth
consumers instead of adding another parser. Public owned conversion signatures
and their affinity, function-typing, CAST, arithmetic, bitwise and truth
policies stay unchanged.

Use the function borrow in SUM/TOTAL/AVG and `sign`. A numeric aggregate still
treats unconverted TEXT/BLOB as a REAL prefix, including REAL zero for
nonnumeric data; `sign` still returns NULL for a nonnumeric type. `abs`
retains its INTEGER overflow and NULL branches and uses the REAL observation
for other storage classes. Preserve its comparison-based negative-zero rule,
not an incidental `fabs` substitution.

These are reached synchronous reads. No borrowed argument escapes a function
call, no public borrowed-result API is added, and function/aggregate outputs
keep their current ownership. MIN/MAX winner copies, text results, separator
conversion, state allocation and finalization remain unchanged.

### Supersede only the retired clone boundary

ADR-0100 deliberately retained explicit aggregate argument cloning while
fixing numeric typing. This ADR supersedes only that ownership decision and
its two-allocation cold numeric expectation; all typing and lifecycle
semantics remain authoritative.

Retain every existing numeric input and failure-injection window. The former
two-allocation ceiling is not raised: the current cold path must allocate
exactly once for lazy aggregate state, and warmed numeric steps allocate
zero times, including long TEXT/BLOB. Exhaust the remaining state-allocation
failure and verify reset/reuse. Do not manufacture a replacement allocation
to preserve the retired clone count.

Keep the independent exact Count, MIN, concatenation and first-separator
allocation controls, public owned-conversion behavior, 36/37 lowering caps,
instruction/value layouts, memory/file budgets and deadlines unchanged.

## Qualification and delivery

Use one coherent value/function ownership source after accepted design.
Begin with the existing fully qualified production baseline, identical across
PRs #251 and #252 except documentation. Observe a focused warmed aggregate
allocation failure on the old clone path before implementation. The shared
value refactor retains green semantics; no artificial SQL failure is needed.

Add owning-module tests for borrowed identity, scratch/argument lifetime,
TEXT/BLOB immutability, function versus arithmetic typing, exact values/REAL
bits, both NUL directions and 51-bit endpoints, int64 bounds, decimal/exponent
and prefix forms, empty/nonnumeric data, signed zero/infinity, overflow-latch
preservation/clearing and scalar controls. Reuse the current allocation
injector, goldens, numeric-type native shard and public session regressions;
do not regenerate an oracle or add a corpus.

Run focused green and complete fast units, the actual change-aware closing
union, unchanged native/model/failure controls, matched fixed work and
ordinary isolated timings. Compare the original global-functions case and
non-target families without changing SQL, fixtures or thresholds. Preserve
flat or adverse measurements instead of claiming a speedup from samples.

Require GPT-6.1 Sol design/implementation review, a normal buildable commit
and append-only published evidence. Final exact-head review, required actual
hosted gates, guarded squash and named cleanup precede the next slice.
Function inventory closure requires its own supported disposition; this
ownership correction alone does not complete node 43 or the project.
