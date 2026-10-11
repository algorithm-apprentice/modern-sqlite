# ADR-0067: Global Aggregate Access Lowering Module

- Status: Accepted
- Date: 2026-10-12

## Context

ADR-0063 added every global aggregate access shape to
`src/lowering/plan_lowering.cpp`: Empty, SingleRow, table scan, rowid lookup,
covering index, noncovering index, rowid MIN/MAX, and indexed MIN/MAX with
reverse traversal and nullable fallback.

The implementation is correct and fully tested, but the file now exceeds
12,000 lines. The global aggregate access region alone owns roughly 1,700
lines of cursor positioning, range checking, candidate stepping, cursor-state
normalization, and MIN/MAX NULL-region control flow. Keeping that cohesive
algorithm inside the general statement lowerer makes the file harder to
review and would force grouped aggregate work to grow the same monolith.

This task is a behavior-preserving extraction. Grouped execution, aggregate
modifiers, public contracts, bytecode, VM behavior, and unrelated lowering
cleanup are outside its scope.

The pre-refactor baseline is green:

- five focused global aggregate lowering and execution tests;
- exhaustive plan-lowering allocation failure;
- exact successful allocation budgets; and
- the merged package-11 compatibility and CI evidence.

## Decision

### 1. Add one internal access-emission module

Create:

```text
src/lowering/
  global_aggregate_access.hpp
  global_aggregate_access.cpp
  plan_lowering_internal.hpp
```

The module owns:

- global access-shape dispatch;
- Empty, SingleRow, table, rowid, covering-index, and noncovering-index
  emission;
- indexed MIN/MAX traversal and runtime metadata;
- nullable MIN/MAX policy and NULL-region passes;
- cursor positioning, range-end checks, table lookup, advance, close, and
  normalized exit schedules; and
- the final access-kind and strategy rejection boundaries.

`plan_lowering.cpp` retains:

- physical-chain inspection;
- register and descriptor layout;
- bound expression emission;
- LIMIT/OFFSET initialization;
- aggregate open, ordinary step, finalize, close, HAVING, and projection;
- shared index-key expression and key-block emission; and
- all nonaggregate and mutation lowering.

### 2. Use a narrow borrowed input record

`GlobalAggregateAccessInput` contains only borrowed immutable plan and binding
contracts plus already allocated bytecode identities:

- `ProgramBuilder`;
- the bound SELECT, expressions, aggregate functions, and invocations;
- the selected access, filter, limit, and aggregate nodes;
- aggregate descriptor and argument-register mappings;
- cursor IDs;
- index-key register layout;
- the noncovering rowid register; and
- the interned NULL constant used by indexed MIN/MAX.

The record owns no plan data, strings, vectors, cursors, or runtime state. It
is valid only for one synchronous emission call.

### 3. Delegate only existing general lowering operations

`PlanLowerer` privately implements a
`GlobalAggregateAccessServices` interface. The access module may request only:

- expression emission;
- LIMIT/OFFSET initialization;
- source-independent aggregate guards;
- residual filters;
- aggregate open and ordinary step;
- common aggregate completion;
- index-key expression evaluation; and
- index-key block materialization.

The interface uses strongly typed arguments and `LoweringResult` values. It
does not expose `PlanLowerer`, unrelated mutable state, or a generic callback
map.

`plan_lowering_internal.hpp` retains the existing lowering result alias,
checked optional access, error construction, and `ProgramResult` conversion
helpers used by both translation units. The extraction does not duplicate
those canonical diagnostics or mappings.

Virtual dispatch is confined to compile-time plan lowering. The interface and
input record allocate no memory, so existing allocation and OOM contracts
remain exact.

### 4. Preserve exact emitted behavior

The extraction changes no:

- instruction order or operands;
- labels or cursor-state joins;
- expression evaluation order;
- LIMIT/OFFSET, guard, filter, HAVING, or projection schedule;
- MIN/MAX traversal, range, equality, or NULL fallback rule;
- lowering error code or diagnostic text;
- register, constant, symbol, descriptor, or cursor count; or
- public header or dependency direction.

Existing focused, allocation, OOM, session, differential, sanitizer, and
compatibility tests remain the behavioral specification. No artificial
failing behavior test is required for this refactor.

### 5. Do not generalize ahead of grouped execution

The new module is specifically the accepted global aggregate access emitter.
Package 12 may reuse a proven operation only when its grouped schedule has the
same invariant. It must not turn this extraction into a generic aggregate
framework or move grouped behavior into the module speculatively.

## Rejected alternatives

### Split the complete `PlanLowerer` class

Rejected because it would mix this focused extraction with every read,
compound, ordering, DML, DDL, and ANALYZE path.

### Include an implementation fragment inside the class

Rejected because an `.inc` file would reduce visible file length without
creating an independently compiled module or an explicit dependency seam.

### Pass `std::function` callbacks

Rejected because type-erased callbacks may allocate and would perturb exact
lowering allocation and OOM tests.

### Expose the complete lowerer through friendship

Rejected because the new module would remain coupled to unrelated mutable
state and could silently expand into another god component.

### Combine the extraction with grouped aggregates

Rejected because behavior-preserving modularization and new execution
semantics require separate review and validation.

## Consequences

### Positive

- Global aggregate access control flow has one cohesive implementation file.
- The general plan-lowering file shrinks without changing behavior.
- Grouped execution can be designed against an explicit boundary rather than
  extending a 12,000-line monolith.
- The service seam mechanically limits cross-module coupling.

### Negative

- Plan lowering gains one private virtual interface.
- The access module still depends on several established lowering services
  because duplicating expression, LIMIT, and key emission is forbidden.
- The indexed MIN/MAX implementation remains intrinsically substantial, but
  its complexity is isolated to its actual responsibility.

## References

- ADR-0005: Test-Driven Development
- ADR-0036: Physical Read Plan Lowering
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0057: SQLite-Aligned Aggregate Roadmap
- ADR-0061: SQLite-Aligned Aggregate Planning
- ADR-0063: SQLite-Aligned Global Aggregate Execution
