# ADR-0012: Optional Instrumentation

- Status: Accepted
- Date: 2026-10-03

## Context

Performance work needs fixed-work counters that explain allocations, copying,
storage access, cache behavior, comparisons, virtual-machine execution, and
planner effort. Those diagnostics must not change the cost or dependencies of
the ordinary engine build.

Scoped collection is also required so one operation cannot accidentally add
measurements to an earlier or concurrent operation.

## Decision

- Instrumentation is a standard-library-only leaf module with no logging,
  persistent mutation, engine policy, dynamic allocation, or locking.
- The ordinary `modern_sqlite` target compiles instrumentation hooks out by
  default and does not link the instrumentation runtime.
- `modern_sqlite::instrumentation` is a separate opt-in runtime target.
- `MODERN_SQLITE_RECORD_COUNTER(counter, amount)` is the only call-site hook.
  In disabled builds it still type-checks its arguments but does not evaluate
  them or reference the runtime.
- Counters cover allocations, bytes copied, VFS calls, pages read and written,
  cache hits and misses, B-tree comparisons, VM instructions, and planner
  work. Their stable names are part of diagnostic report compatibility.
- `CounterCollection` owns fixed-size unsigned counters and can be explicitly
  reset and reused.
- `ScopedCounterCollection` installs one active collection for the current
  thread and restores the previous collection on destruction. Nested scopes
  are supported, and records outside a scope are discarded.
- A collection is owned by one thread while active. Instrumentation does not
  add synchronization to engine hot paths.
- Instrumented timings are diagnostic only and cannot be used as performance
  parity claims.

## Consequences

- Ordinary builds have no instrumentation branches, argument evaluation,
  runtime symbols, or linked instrumentation state.
- Instrumented builds pay for a thread-local lookup and counter update at each
  enabled hook.
- Operation and thread boundaries remain explicit and testable.
- The macro boundary is accepted because it is the only way to guarantee that
  disabled hook arguments are not evaluated in unoptimized builds.
