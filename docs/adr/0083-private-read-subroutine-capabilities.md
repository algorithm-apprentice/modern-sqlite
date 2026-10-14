# ADR-0083: Private Read Subroutine Capabilities

- Status: Accepted
- Date: 2026-10-14

## Context

PR #193 verifies static value/control calls but intentionally rejects storage
and aggregate capabilities inside callable regions. Scalar and EXISTS query
bodies need their own read cursors and temporary consumers before SQL
subquery grammar can be admitted.

Analyzing a body with closed capabilities is not sufficient by itself: a body
could reopen a cursor ID that its caller already owns. The static call
register summary would then preserve an incorrect caller cursor state.
Lexical resource ownership must therefore precede read-capability acceptance.

The existing operand checker already identifies every read/write cursor,
sorter, top-N, relation, and aggregate reference. The existing dataflow lattice
already proves their exact lifecycles. Reuse both rather than introducing
another instruction-resource visitor or approximate capability checker.

## Decision

### 1. Opt in through the existing immutable descriptor

Append a typed subroutine kind with `Value` as the unchanged default and
`Read` as the new explicit opt-in. Preserve descriptor cloning, builder-owned
labels, owned-byte accounting, static call graph, register summaries, and
typed continuations.

A read body admits the existing read-cursor, sorter, top-N, keyed relation,
and aggregate instructions in addition to value/control operations.
Persistent mutation, schema operations, write cursors, the statement-global
rowid list, client result rows, and halt remain forbidden.

This adds no VM opcode, parser acceptance, runtime compiler, persistent format,
or separate temporary-storage factory.

Value subroutines may call only value subroutines. Read subroutines may call
either kind. The explicit opt-in cannot be bypassed by an indirect read call.

### 2. Prove one lexical owner for every mutable capability

Every referenced mutable descriptor belongs to exactly one lexical region:
main or one subroutine. Distinct descriptors may read the same physical table,
but no descriptor ID can cross owners.

Extend the canonical operand-check callbacks with an analysis-only ownership
view. The same cursor/sorter/top-N/relation/aggregate ID validation records and
checks ownership. Immutable symbols, constants, and record-comparison
descriptors remain shareable. Do not duplicate the resource-reference list.

Charge the ownership map to the existing checked analysis budget. Allocate
it only when a read subroutine is present; the ordinary and value-only
preparation paths retain their previous allocation behavior.

Caller-owned cursors cannot be borrowed, repositioned, or closed by a body.
Correlation passes SQL values through the existing register interface.
Capability borrowing or sharing requires a later explicit contract.

### 3. Require closed private resources on every normal return

Analyze every body through the canonical capability transfer and merge rules.
All private cursors and temporary/aggregate slots must be closed on return.
In particular, finalize and close an aggregate before returning so delayed
errors cannot disappear.

Nested calls preserve the caller's current resources while their own disjoint
resources are closed on entry and exit. Main retains its existing halt and
cleanup semantics; this package does not tighten unrelated halt rules.

The runtime uses the existing call stack, cursor slots, aggregate states, and
statement-owned temporary storage. No resource frame or alternate handle
container is added. An I/O, corruption, budget, OOM, scalar error, interruption,
reset, or destruction follows canonical whole-execution cleanup. Normal
return neither closes caller resources nor silently repairs a malformed body.

### 4. Verify ownership and failure behavior in the owning modules

Observe a focused read-call contract rejection before implementing it.
Cover normal/empty sources, nested and repeated calls, every private
capability family, caller state preservation, cross-owner use, unknown kinds,
forbidden writes/global operations, open/pending/unfinalized return states,
exact budgets, failure injection, reset/rebind, and allocation boundaries.

Use the existing SQLite fixtures, VFS fault helpers, temporary storage, and
aggregate implementations. Keep all frozen SQL oracles, workloads, thresholds,
and allocation budgets unchanged where behavior is unchanged. Resource-aware
coroutines and actual SQL subquery syntax remain separate reviewed slices.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 subquery-control prerequisite.

Contracts: ADR-0031, ADR-0032, ADR-0048, ADR-0053, ADR-0056, ADR-0058,
ADR-0062, ADR-0076, and ADR-0082.
