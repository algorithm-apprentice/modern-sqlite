# ADR-0062: Typed Aggregate Bytecode and VM State

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0061 publishes aggregate physical plans but deliberately stops lowering
with `aggregate lowering is not implemented`. Package 10 must add the typed
bytecode and runtime capability that later global and grouped lowering will
target, without yet making aggregate SQL executable through the session.

The merged prerequisites provide:

- immutable bytecode with strong resource IDs, descriptor tables, register
  dataflow verification, and branch-state merging;
- VM-owned cursor, sorter, top-N, and relation capability slots with cleanup
  on halt, error, reset, detach, move assignment, and destruction;
- synchronous scalar function and collation resolution during `Vm::Create`;
- immutable aggregate descriptors plus move-only `AggregateState`; and
- aggregate planning metadata for invocation identity, collation, cardinality,
  representative effects, and grouping strategy.

Pinned SQLite 3.54.0 defines the relevant execution mechanism in:

- `src/vdbe.c:7982-8130`, where aggregate step binds one function context to
  an accumulator and reads a contiguous argument range;
- `src/vdbe.c:8132-8185`, where finalization handles both stepped and empty
  aggregate state and publishes one value; and
- `src/vdbemem.c:506-575`, where aggregate payload cleanup occurs during
  finalization or ordinary VM memory cleanup.

Modern SQLite already owns the function-specific semantics in ADR-0059.
Package 10 therefore needs no parser, binder, planner, physical-plan,
temporary-storage, or session type inside bytecode or VM.

## Decision

### 1. Add one typed aggregate descriptor and strong ID

Append:

```cpp
struct AggregateIdTag;
using AggregateId = BytecodeId<AggregateIdTag>;

struct AggregateDescriptor {
  SymbolId function;
  SymbolId collation;
  std::uint32_t argument_count = 0;

  constexpr auto operator<=>(const AggregateDescriptor&) const noexcept =
      default;
};
```

The function and collation are connection-independent symbols. The descriptor
contains no registry pointer, runtime state, bound invocation ID, syntax,
plan, register allocation, sorter, or representative-row storage.

Every invocation receives its own descriptor and runtime state slot even when
multiple invocations resolve to the same built-in function. Later lowering
may reuse one descriptor only when it also intends to reuse the same
accumulator lifecycle.

`ProgramInput`, `BytecodeProgram`, and `ProgramBuilder` append aggregate
descriptor storage and immutable accessors. `ProgramLimits` appends
`maximum_aggregates`, defaulting to 100,000. The new fields are appended to
preserve existing positional aggregate initialization.

### 2. Append five aggregate lifecycle instructions

Append these instruction alternatives and kind values:

```cpp
enum class BytecodeAggregateStepEffect : std::int64_t {
  kNoRepresentativePreference = 0,
  kKeepRepresentative = 1,
  kReplaceRepresentative = 2,
};

struct OpenAggregateInstruction {
  AggregateId aggregate;
};

struct StepAggregateInstruction {
  AggregateId aggregate;
  RegisterId first_argument;
  std::optional<RegisterId> effect_output{};
};

struct FinalizeAggregateInstruction {
  AggregateId aggregate;
  RegisterId output;
};

struct ResetAggregateInstruction {
  AggregateId aggregate;
};

struct CloseAggregateInstruction {
  AggregateId aggregate;
};
```

The descriptor supplies the argument count. Zero-argument COUNT therefore
uses an empty span and does not require an initialized argument register.

`StepAggregateInstruction` writes the integer value of
`BytecodeAggregateStepEffect` only when `effect_output` is present. The write
occurs after a successful step, so the output may overlap an argument
register. Later representative-row lowering may branch on the value; ordinary
aggregates omit the output.

`FinalizeAggregateInstruction` finalizes both stepped and empty state and
writes the result only on success.

All instruction alternatives and kind values are appended, preserving every
existing numeric kind and variant index.

### 3. Verify descriptors without the function registry

Bytecode verification checks:

- descriptor function and collation symbols are in range;
- aggregate count respects `maximum_aggregates` and 32-bit strong IDs;
- every aggregate instruction references a valid descriptor;
- step arguments form an initialized contiguous range of exactly the
  descriptor's argument count;
- optional step-effect and final-result registers are in range; and
- aggregate descriptor bytes participate in retained/logical owned-byte
  accounting, cloning, builder reservation, and allocation limits.

The verifier does not decide whether the named function exists or whether its
registered arity matches. Those are runtime-environment checks in
`Vm::Create`, matching scalar-call resolution.

Append typed program errors for:

- aggregate count limit;
- invalid aggregate ID; and
- invalid aggregate descriptor.

Existing error-code numeric values remain unchanged.

### 4. Track one closed/accumulating/finalized state per aggregate

The control-flow verifier tracks:

```text
Closed --Open--> Accumulating
Accumulating --Step--> Accumulating
Accumulating --Finalize--> Finalized
Finalized --Reset--> Accumulating
Finalized --Close--> Closed
```

Rules are:

- opening an open slot is `kCapabilityAlreadyOpen`;
- closing a closed slot is `kCapabilityAlreadyClosed`;
- step, finalize, or reset on a closed slot is `kCapabilityNotOpen`;
- step or finalize after finalization without reset is
  `kCapabilityStateConflict`; and
- reset or close while still accumulating is `kCapabilityStateConflict`;
- successful Halt requires every aggregate slot to be Closed or Finalized; and
- branches may merge only when every aggregate slot has the same lifecycle
  state, using the existing capability-state conflict error.

Programs may halt with finalized aggregate slots because finalization has
already surfaced delayed SUM and concatenation failures. They may not
successfully discard an accumulating slot: ADR-0059 reset/destruction would
erase latched overflow, too-large, or OOM state. VM-owned destructive cleanup
remains mandatory for an independent execution error, interruption, public
reset, detach, move assignment, and destruction, so explicit close is not
required after successful finalization.

Step initializes `effect_output` when present. Finalize initializes its output
register. Reset and close initialize no register.

### 5. Resolve aggregate functions and collations during VM creation

`Vm::Create` resolves every descriptor exactly once:

```cpp
functions.ResolveAggregate(program.symbol(descriptor.function),
                           descriptor.argument_count);
ResolveCollation(descriptor.collation);
```

Unknown names, wrong arity, and missing collations preserve their existing
typed runtime errors. The VM stores stable borrowed pointers to the resolved
`AggregateFunction` and `Collation`; the registry descriptor array and
collation objects must outlive the VM under the same lifetime rule as scalar
calls.

The immutable bytecode retains symbols only. It never stores runtime pointers.

### 6. Own move-only aggregate states in VM capability slots

Each VM aggregate slot contains:

- the resolved immutable function pointer;
- the resolved collation pointer; and
- an optional move-only `AggregateState`.

Execution is:

- Open: create the descriptor-bound nonallocating state wrapper;
- Step: borrow the contiguous register arguments, construct
  `AggregateFunctionContext` from the resolved collation and VM value limit,
  call `AggregateFunction::Step`, then optionally publish the representative
  effect;
- Finalize: call `AggregateFunction::Finalize` and move the successful result
  into the destination register;
- Reset after finalization: call `AggregateState::Reset`; and
- Close after finalization: destroy the state wrapper.

The VM performs no function-specific branching for COUNT, numeric aggregates,
MIN/MAX, or concatenation. It only maps `AggregateStepEffect` to the bytecode
effect integers.

Step and final errors propagate unchanged. Destination registers are replaced
only after successful callbacks. Allocation failure follows the existing VM
conversion to `kOutOfMemory`.

### 7. Extend unconditional VM cleanup

The existing cleanup path also destroys every live aggregate state before
clearing statement temporary storage.

This applies to:

- successful Halt;
- any execution error;
- instruction-budget interruption;
- reset from Ready, Row, Done, or Error;
- execution-context detach;
- VM move assignment; and
- destruction.

`Vm::Reset` closes aggregate slots rather than preserving their open state.
The next execution must run bytecode Open instructions again. It retains
parameter bindings and resolved function/collation pointers exactly as today.

### 8. Keep SQL lowering explicitly unsupported

Package 10 does not change the ADR-0061 lowering guard. Aggregate physical
plans still return:

```text
aggregate lowering is not implemented
```

All new behavior is verified through hand-authored typed programs. Package 11
will allocate descriptors and emit global aggregate lifecycles.

Delivery update: ADR-0063 now allocates those descriptors and emits the
accepted ungrouped global and indexed-MIN/MAX lifecycles. This package's
direct-program boundary remains the historical scope of package 10.

### 9. Verify lifecycle, state semantics, errors, and allocation

Development follows red-green-refactor. Tests cover:

- strong aggregate-ID separation and appended stable instruction names;
- descriptor cloning, builder insertion, limits, owned bytes, invalid symbols,
  invalid IDs, register ranges, and zero-argument steps;
- open/step/finalize/reset/close lifecycle and branch-state conflicts;
- rejection of accumulating reset, close, and successful halt paths so
  delayed aggregate errors cannot be discarded;
- initialized effect and result registers;
- VM creation with exact name/arity and collation resolution;
- empty finalization, repeated stepping, reset reuse, and finalization
  one-shot behavior;
- COUNT, SUM, MIN/MAX representative effects, and GROUP_CONCAT delayed errors
  through direct programs;
- destination overlap with argument registers;
- cleanup on halt, callback error, reset, instruction interruption, move, and
  destruction;
- exact allocation budgets and exhaustive bytecode/VM OOM cuts; and
- allocation-free immutable descriptor and instruction access.

No aggregate SQL lowering, grouping loop, representative snapshot,
DISTINCT/FILTER/local ordering schedule, sorter integration, or session result
is implemented in this package.

## Consequences

### Positive

- Aggregate runtime state has the same typed, verified ownership boundary as
  sorters and relations.
- Function-specific semantics remain isolated in the registry state module.
- Later lowering can compose global and grouped execution without adding
  untyped VM storage or runtime lookup in the dispatch loop.
- Representative-row decisions are explicit bytecode data rather than hidden
  VM flags.

### Negative

- Programs gain another descriptor table and verifier state word per
  aggregate slot.
- Representative effects use integer register values at the bytecode
  boundary.
- Aggregate function and collation descriptors must outlive every VM that
  resolved them.

## Rejected alternatives

### Store aggregate state in ordinary SQL registers

Rejected because registers own `SqlValue`, while aggregate state is
descriptor-bound move-only runtime capability with a different lifecycle.

### Resolve aggregate names on every step

Rejected because immutable registration is already resolved once for scalar
calls and repeated lookup would add hot-loop cost and suspended-execution
instability.

### Put runtime aggregate pointers in bytecode

Rejected because bytecode is connection-independent and outlives individual
runtime environments.

### Infer representative effects from final values

Rejected because MIN/MAX step decisions control source snapshots before
finalization and cannot be reconstructed from the final extremum alone.

### Omit explicit open/reset/close

Rejected because grouped execution reuses invocation slots across group
boundaries and the verifier must prove one exact lifecycle.

## References

- ADR-0003: Dependency Direction and Layering
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0031: Typed Immutable Bytecode Programs
- ADR-0032: Read-Only Bytecode Virtual Machine
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0059: SQLite-Aligned Aggregate Function State
- ADR-0061: SQLite-Aligned Aggregate Planning
