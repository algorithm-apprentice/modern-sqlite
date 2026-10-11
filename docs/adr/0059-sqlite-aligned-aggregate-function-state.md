# ADR-0059: SQLite-Aligned Aggregate Function State

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0057 package 7 adds aggregate descriptors and state semantics before
aggregate binding, planning, bytecode, or execution. The merged prerequisites
already provide:

- move-only `SqlValue` storage classes, numeric affinity, casts, and exact
  integer/REAL comparison;
- immutable collation objects and collation-aware value comparison;
- an immutable scalar-function registry with exact and minimum arities; and
- parsed COUNT, SUM, TOTAL, AVG, MIN, MAX, GROUP_CONCAT, STRING_AGG,
  DISTINCT, FILTER, and function-local ordering syntax.

The package must not add a private VM, binder object, group container, sorter,
or session dependency. It must nevertheless fix the state ownership and
observable callback behavior that later aggregate bytecode will consume.

Pinned SQLite 3.54.0 defines the relevant mechanism in:

- `src/func.c:1870-2058`, for integer accumulation,
  Kahan-Babuska-Neumaier summation, overflow latching, and SUM/TOTAL/AVG
  finalization;
- `src/func.c:2070-2112`, for zero- and one-argument COUNT;
- `src/func.c:2119-2177`, for MIN/MAX state, collation, strict replacement,
  and accumulator-load suppression;
- `src/func.c:2180-2340`, for GROUP_CONCAT/STRING_AGG lazy state, separator
  ordering, length limits, and delayed buffer errors;
- `src/func.c:3410-3482`, for scalar/aggregate MIN/MAX coexistence and core
  aggregate arities; and
- `src/vdbe.c:8000-8180`, for statement-local aggregate context allocation,
  step errors, empty finalization, and final-state destruction.

Pinned behavior probes additionally establish:

- empty COUNT is integer `0`, empty TOTAL is REAL `0.0`, and the other empty
  aggregates are NULL;
- numeric TEXT can preserve integer SUM, nonnumeric TEXT/BLOB contributes
  REAL `0.0`, and mixed numeric input uses REAL accumulation;
- an integer SUM overflow is reported only at finalization and is cleared by
  a later noninteger input;
- TOTAL and AVG do not report the integer-overflow latch;
- opposite infinities finalize through NaN to SQL NULL;
- aggregate MIN/MAX retain the first comparator tie and its storage class;
  and
- a NULL GROUP_CONCAT separator is empty, NULL values are omitted, and each
  accepted row's separator precedes its value.

## Decision

### 1. Extend the immutable registry with aggregate descriptors

`FunctionRegistry` borrows independent immutable spans of scalar and aggregate
descriptors. Existing scalar construction remains source compatible by
defaulting the aggregate span to empty.

The aggregate surface is:

```cpp
enum class AggregateExtremumKind : std::uint8_t {
  kNone,
  kMinimum,
  kMaximum,
};

enum class AggregateStepEffect : std::uint8_t {
  kNoRepresentativePreference,
  kKeepRepresentative,
  kReplaceRepresentative,
};

class AggregateFunctionContext final {
 public:
  AggregateFunctionContext(
      const Collation& collation,
      ByteCount maximum_value_bytes) noexcept;

  [[nodiscard]] const Collation& collation() const noexcept;
  [[nodiscard]] ByteCount maximum_value_bytes() const noexcept;
};

class AggregateState final {
 public:
  AggregateState(const AggregateState&) = delete;
  AggregateState& operator=(const AggregateState&) = delete;
  AggregateState(AggregateState&&) noexcept;
  AggregateState& operator=(AggregateState&&) noexcept;
  ~AggregateState();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool finalized() const noexcept;
  void Reset() noexcept;
};

class AggregateFunction final {
 public:
  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] FunctionArity arity() const noexcept;
  [[nodiscard]] bool is_deterministic() const noexcept;
  [[nodiscard]] bool uses_collation() const noexcept;
  [[nodiscard]] AggregateExtremumKind extremum_kind() const noexcept;

  [[nodiscard]] AggregateState CreateState() const noexcept;
  [[nodiscard]] Result<AggregateStepEffect> Step(
      AggregateState& state,
      const AggregateFunctionContext& context,
      std::span<const SqlValue> arguments) const;
  [[nodiscard]] Result<SqlValue> Finalize(AggregateState& state) const;
};
```

`FunctionRegistry::ResolveAggregate(name, argument_count)` uses the same ASCII
case folding, exact-arity preference, and `no such function` /
`wrong number of arguments` error wording as scalar resolution. Scalar and
aggregate lookup remain separate, so scalar MIN/MAX with two or more
arguments coexist with aggregate MIN/MAX of exactly one argument.

The core aggregate descriptors are:

| Name | Arity | Collation | Extremum |
|---|---:|---|---|
| `count` | 0 | no | none |
| `count` | 1 | no | none |
| `sum` | 1 | no | none |
| `total` | 1 | no | none |
| `avg` | 1 | no | none |
| `min` | 1 | required | minimum |
| `max` | 1 | required | maximum |
| `group_concat` | 1 | no | none |
| `group_concat` | 2 | no | none |
| `string_agg` | 2 | no | none |

All are deterministic. User-defined aggregate construction, connection-local
replacement, inverse/value callbacks, subtypes, and security flags remain
deferred.

Aggregate descriptors are non-copyable and non-movable. Their stable address
is the state-binding identity, so a live state's function semantics cannot be
reassigned in place.

### 2. Use one move-only state wrapper with hidden typed payloads

`AggregateState` borrows its immutable descriptor and owns an optional hidden
implementation through `std::unique_ptr`. The implementation is a closed
variant of the core COUNT, numeric, MIN/MAX, and concatenation payloads.
There is no owning raw pointer, public `void*`, parser object, VM object, or
catalog object.

`CreateState()` creates only the nonallocating wrapper. Function-specific
payload allocation is lazy:

- COUNT, SUM/TOTAL/AVG, and MIN/MAX allocate on their first step, including a
  NULL step;
- GROUP_CONCAT/STRING_AGG skip a NULL value before allocation; and
- finalization without a payload calls the descriptor's explicit empty-final
  behavior.

The descriptor must outlive every state created from it. Core descriptors have
process lifetime.

Move construction and assignment invalidate the source. Using an invalid
state, a state created by another descriptor, stepping after finalization, or
finalizing twice returns `kMisuse`. `Reset()` destroys the payload and latched
error, clears finalization, and preserves the descriptor binding for reuse.

Finalization is one-shot. Success or failure destroys the payload and marks
the wrapper finalized. Explicit destruction at any lifecycle point is
infallible.

### 3. Report representative-row effects from every step

Every successful step returns one `AggregateStepEffect`.

Ordinary aggregates return `kNoRepresentativePreference`. MIN/MAX return:

- `kReplaceRepresentative` for the first non-NULL value, every strict new
  extremum, and every NULL before a non-NULL winner exists;
- `kKeepRepresentative` for comparator ties, worse values, and NULL after a
  winner exists.

This reproduces SQLite's accumulator-load suppression and allows later
execution to keep the last source row for an all-NULL MIN/MAX group. A later
package decides which MIN/MAX invocation controls the group snapshot;
package 7 owns only the per-step effect.

### 4. Keep immediate and delayed failures distinct

Expected failures use typed `Result` values.

These failures are immediate step failures:

- function-state allocation failure;
- argument cloning or text/numeric conversion allocation failure;
- MIN/MAX winning-value clone failure;
- invalid state or arity; and
- the defensive, practically unreachable COUNT overflow check.

GROUP_CONCAT/STRING_AGG retain SQLite's delayed result-buffer failures:

- exceeding `maximum_value_bytes` latches `kTooLarge` with
  `string or blob too big`;
- result-buffer allocation or length failure latches `kOutOfMemory`;
- later rows still perform required argument conversions, but no later buffer
  mutation replaces the first latched error; and
- finalization returns the latched error.

The maximum applies to resulting text bytes, not the unused first separator.
An exact-limit result is accepted.

### 5. Implement COUNT without undefined overflow

`count()` increments for every step. `count(expr)` increments only for
non-NULL input. Empty and all-NULL one-argument input finalize to integer `0`.

The counter is signed 64-bit. Modern SQLite checks before incrementing rather
than relying on signed overflow. Exceeding the representable count returns
`kTooLarge`; reaching that count is not feasible under current database and
statement limits, but the implementation remains standard-defined.

### 6. Share one SQLite-aligned numeric state

SUM, TOTAL, and AVG share:

```text
real sum
real error
signed integer sum
signed count
approximate flag
integer-overflow flag
```

Each non-NULL argument first receives SQLite numeric-type coercion:

- INTEGER remains integer;
- REAL remains real;
- complete integer-form TEXT may become INTEGER, while decimal-point or
  exponent TEXT remains REAL even when its value is integral;
- BLOB and nonnumeric TEXT use REAL conversion, contributing `0.0` when no
  numeric prefix exists; and
- NULL is ignored.

TEXT numeric classification stops at the first embedded NUL before deciding
whether decimal-point or exponent syntax forces REAL.

Integer mode uses checked signed addition. On overflow it:

1. latches overflow;
2. initializes the compensated real state from the prior integer sum;
3. switches permanently to approximate mode; and
4. adds the triggering integer through the split integer path.

Large integers at or beyond magnitude `2^52` are split into a multiple of
16384 plus a small remainder before Kahan-Babuska-Neumaier addition.

In approximate mode, later INTEGER values use that split path. A later
noninteger value clears the overflow latch before it is added. Finalization:

- SUM returns NULL for count zero, INTEGER while exact, REAL while
  approximate, or `integer overflow` when the latch remains set;
- TOTAL always returns REAL and uses `0.0` for count zero; and
- AVG returns NULL for count zero and otherwise REAL sum divided by count.

TOTAL and AVG ignore the integer-overflow latch. If the compensation term is
non-finite, finalization uses the running sum alone. A NaN REAL result becomes
SQL NULL through the existing `SqlValue::Real` contract.

### 7. Preserve aggregate MIN/MAX value ownership and ties

MIN/MAX ignore NULL for their mathematical value. The first non-NULL argument
is cloned into state. Later arguments use `CompareSqlValues` with the
invocation collation:

- MIN replaces only when the candidate is strictly less;
- MAX replaces only when the candidate is strictly greater; and
- equivalent values retain the first winner and its original storage class.

Finalization transfers the owned winner without another payload copy.
No non-NULL winner finalizes to NULL.

### 8. Preserve GROUP_CONCAT and STRING_AGG ordering

A NULL first argument is skipped before state creation or separator
conversion. Every accepted argument is converted to TEXT with the existing
SQLite-compatible cast rules.

For the first accepted value, no separator bytes are appended. For each later
value:

- one-argument GROUP_CONCAT appends `,`;
- two-argument GROUP_CONCAT and STRING_AGG convert that row's separator to
  TEXT and append it; and
- a NULL separator appends zero bytes.

For the two-argument forms, the first row's separator is still converted, as
SQLite's window-capable implementation does, but its bytes are not included.
The value is converted after separator handling. Embedded NUL and malformed
UTF-8 bytes are preserved.

No accepted value finalizes to NULL. One accepted empty string finalizes to
empty TEXT. Finalization transfers the accumulated string into `SqlValue`.

### 9. Verify state behavior independently from SQL execution

Development is red-green-refactor. Package-local tests cover:

- aggregate registry names, case folding, arities, scalar MIN/MAX coexistence,
  metadata, and wrong-arity errors;
- move-only state ownership, moved-from rejection, descriptor mismatch,
  initialization, finalization, reset, and destruction;
- COUNT empty, NULL, zero-argument, one-argument, and boundary behavior;
- SUM/TOTAL/AVG empty, NULL, integer, real, numeric text, nonnumeric
  text/blob, large integer, overflow, overflow clearing, infinity, and
  compensation cases;
- MIN/MAX NULL, all-NULL, numeric-equivalent ties, storage-class ownership,
  collations, strict replacement, and representative effects;
- GROUP_CONCAT/STRING_AGG NULL skipping, default/NULL/varying separators,
  conversion of every storage class, embedded NUL, empty text, exact length
  limits, delayed too-large/OOM errors, and reset;
- exhaustive allocation failure for state creation, winning-value cloning,
  conversion, growth, and finalization cleanup; and
- no allocation or file ownership outside the state object.

ADR-0060 and ADR-0062 later add binder metadata plus typed bytecode/VM state
over this contract. Session-visible SQL execution, DISTINCT, FILTER
scheduling, local ordering, and grouped representative snapshots remain owned
by later packages.

## Consequences

### Positive

- Later binder and VM packages receive one immutable aggregate registry and
  one lifecycle-safe state contract.
- Core semantics are testable without bytecode or grouping machinery.
- SQLite's delayed concatenation errors and MIN/MAX representative gate are
  preserved instead of being reconstructed in lowering.
- State cleanup is deterministic under success, error, reset, move, and OOM.

### Negative

- The closed core payload variant must be extended when a new built-in
  aggregate is accepted.
- Every aggregate step performs descriptor/state validation before dispatch.
- Connection-local aggregate registration requires a later owning descriptor
  and invalidation design.

## Rejected alternatives

### Store aggregate state in the VM register variant

Rejected because it couples runtime functions to VM storage and prevents
independent state tests and registry reuse.

### Use public `void*` state with destroy callbacks

Rejected because it exposes untyped owning storage and weakens the repository's
ownership contract.

### Return NULL immediately after GROUP_CONCAT growth failure

Rejected because SQLite latches result-buffer failures while subsequent rows
continue and reports the error at finalization.

### Convert every numeric input directly to double

Rejected because it loses exact integer SUM results, changes overflow
behavior, and mis-sums large integers.

### Replace compensated summation with the standard library

Rejected because no standard reduction fixes SQLite's step order, split
integer path, overflow latch, or cross-platform result contract.

## References

- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0010: Byte and Buffer Primitives
- ADR-0011: Typed Errors and Results
- ADR-0015: SQL Values, Affinity, and Comparison
- ADR-0016: Collation Contracts and Built-ins
- ADR-0017: Scalar Function Registry
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0057: SQLite-Aligned Aggregate Roadmap
