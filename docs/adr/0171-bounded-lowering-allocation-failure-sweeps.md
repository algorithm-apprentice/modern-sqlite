# ADR-0171: Bounded Lowering Allocation-Failure Sweeps

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0106, ADR-0153, ADR-0164

## Context and measured failure

The storage repairs and native quality follow-up in draft #270 preserve the
engine's allocation-failure workload. Final-head CI at `c27c88f` passes native
Windows and all four actual Full quality jobs, but the sanitizer owner
`lowering.plan_lowering_oom` reaches its unchanged 10-second limit at
10.03 seconds. Its later tests are not execution evidence. A queued macOS
Debug context was cancelled without running and is not a successful check.

The failure is not a rollback-result failure or evidence that the new
durability metadata caused a compiler defect. The same source passes the
local original sanitizer gate in 8.54 seconds. A controlled paired replay
with writable-session OOM takes 7.91 seconds, so local contention is not
established as the cause. Reserving CTest slots alone is not a demonstrated
repair.

The parent source trace disproves repeated fixture construction as the
dominant-loop explanation: the physical plans are already constructed once
before the failure-prefix sweeps. Do not remove that work or describe a
nonexistent setup-hoisting optimization.

An instrumented diagnostic copy, not the ordinary library or qualifying
deadline result, enumerates the original GNU ASan/UBSan population:

- 162 physical read/mutation cases;
- 9,014 allocation-failure prefixes;
- 162 successful recovery calls;
- seven cases requiring lookup-relation publication; and
- the original per-case 128/256/320/512 allocation ceilings.

These are compiler-specific observed counts, not new portable hardcoded
allocation limits. Preserve each fixture, ceiling, lookup check, prefix
exception assertion and recovery. No production lowerer, parser, verifier,
allocator or SQL behavior changes in this repair.

## Existing canonical mechanism

ADR-0164 already qualifies two bounded compiler-test workers with explicit
thread-local allocation domains in `physical_plan_oom_test.cpp`. That owner
registers every participating thread, observes ordinary and aligned
allocations, rejects unregistered allocation throughout the real owner, joins
both workers and retains its whole 10-second deadline. Its CTest declaration
reserves two processor slots.

Lowering takes immutable physical plans by const reference. Its independent
cases retain their already built owners throughout the sweep; they use
initialized immutable catalog/function/collation state and no filesystem,
connection or mutable cursor. Builder label identities are already atomic.
The test can schedule these independent cases without admitting concurrent
SQL connections or another DAG node.

The canonical allocation-domain semantics must be shared, not copied into a
second local implementation.

## Decision

### 1. Extract the existing allocation fault domain for its two consumers

Move the optimizer's allocation index, failure index, injection flag,
registration, activation and unexpected-thread observation into one narrow
test-support header. Keep the existing ordinary/aligned allocation and
recovery semantics.

Optimizer uses the same shared domain without changing any fixture, exact
9/7/etc allocation assertion, ordering or worker count. Its timing state
remains owner-specific and its existing isolation check remains represented.
Lowering uses that same mechanism.

Main registers before activating observation. Every worker registers before
compiler work. Activation and unexpected-thread observation remain in force
through serial setup, worker execution, joining and final success checks.
Do not clear an unexpected allocation to manufacture a pass.
Pre-main runtime initialization remains outside the owner.

### 2. Run the entire lowering population through two bounded lanes

Construct the same immutable fixtures once. Deterministically assign each
original case to exactly one of two workers while preserving that lane's
case order. A case performs its baseline/ceiling/lookup checks, every
allocation-failure prefix and successful recovery on the same registered
thread. Do not move failure injection into another thread or share a
counter across workers.

Keep the existing case dispatch and plan owners canonical. Account for all
162 case identities exactly once and join both workers before returning.
Report per-case baseline allocation counts, attempted prefixes, recovery and
lookup requirements so the parent can compare the before/after manifest
under each unchanged compiler configuration. Reject missing, duplicated,
failed or unobserved work explicitly.

Worker exceptions reset only their own injection state and fail the owner.
They are not successful skips. The main thread's state remains independent.

Declare `PROCESSORS 2` for the whole lowering sweep and its allocation-domain
control, as for the existing optimizer owner. The original owner name,
command, labels, 10-second wall deadline, sanitizer flags and failure
semantics remain. There is still one complete lowering owner, not two
permitted 10-second shards.

### 3. Prove the fault domains before enabling worker scheduling

First add a focused control that fails against the old process-global
lowering state: a worker must not observe the main counter/failure/injection
storage. The completed shared control also checks ordinary and aligned
unregistered-thread rejection, registered first-allocation failure, recovery
and unchanged main-thread state.

Retain the optimizer's additional owner-specific timing-domain check.
Controls run in their own processes so intentional rejection cannot reset
the actual sweep's unexpected-thread observation.

## Qualification and scope limits

Before implementation require independent design acceptance. Then record the
genuine control red, preserve the original fixture-construction source block
byte-for-byte, and compare the complete before/after case/prefix/recovery/
lookup/ceiling manifests under the same compiler.

Require the unchanged whole-owner 10-second sanitizer gate, both owners'
thread-domain controls, optimizer exact allocation assertions, complete fast
units and the selected original model/native/quality checks. A successful
ordinary run must not omit a worker, case, prefix or recovery.

This is bounded verification scheduling with explicit resource ownership,
not an engine speedup or a single-CPU timing comparison. If lowering reaches
shared mutable compiler state, do not assume independence; stop and review
that mechanism before scheduling it.

Deliver as one localized final-gate remediation commit in #270 under
ADR-0106. The four-commit batch is then closed to further accumulation:
another significant gate finding requires an explicitly reviewed split, not
another unbounded series of progress patches or a blind retry.
Final integrated qualification runs against the final exact batch head.

## Local work reconciliation

The completed two-lane owner preserves the original GNU sanitizer manifest
case by case: all 162 identities, 9,014 allocation-failure prefixes, 162
recoveries, seven lookup requirements and every per-case ceiling match.
The unchanged whole-owner 10-second gate completes in 4.90 seconds;
the existing optimizer owner completes in 7.80 seconds with its exact
assertions retained. Both independent thread-domain controls pass.
These are verification-scheduling observations, not engine timings.

Allocation-hook diagnostics retain direct C stdio. Potentially allocating
C++ formatting could recursively enter the fault allocator or alter the
observed prefix. The shared fault header and its two allocator-hook owners
therefore document a source-local `modernize-use-std-print` exception.
This exception affects only that modernization suggestion; ordinary compiler
format checking, analyzer/bugprone checks and warning-as-error policy remain.
It is not a suppression of a lifetime, failure-coverage or runtime finding.
