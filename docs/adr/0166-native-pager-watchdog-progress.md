# ADR-0166: Native Pager Watchdog Progress

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Related decisions: ADR-0106, ADR-0153, ADR-0165
- Base: PR #268 squash `d5ef969`, tree `41017ff2`
- Design accepted before test implementation at proposal `e24eb6b7`.

## Observed actual-main failure

The exact reviewed PR #268 head passed all nine CI contexts and all four
manual Full quality jobs. The guarded squash preserves its exact source
tree. Actual-main native rebuilding, the complete 2,174-case correctness
selection and two tooling owners also pass locally, with the freshly built
ordinary 1 MiB unit-test stack.

Actual-main CI run 40841944328 nevertheless stops its native Windows
selection at `WritePager.PosixVfsCommitsAndRecoversAHotJournal`. Its original
10-second watchdog reports 11.38 seconds elapsed. The other eight jobs pass,
including the sanitizer job. The captured GoogleTest output identifies the
test but no completed filesystem, pager or recovery phase.

The same actual-main local case takes 0.07394 seconds. A bounded diagnostic
of 40 isolated executions under the original per-case deadline takes
0.07-0.09 seconds each. It does not reproduce the hosted failure or turn the
failed hosted gate into a pass.

The test uses the real `NativeVfs`, a private Unicode database path, normal
commit/readback, an explicitly retained hot journal and writable recovery.
Windows I/O retries, locks, synchronization and cleanup retain ADR-0153's
native mapping. No observation identifies the blocked operation, establishes
a storage defect or proves a host cause.

## Decision

Add test-owned, immediately flushed progress before and after the existing
temporary-file, open, read/write-transaction, commit, spill, close and recovery
phases. Include monotonic elapsed and preceding-phase time. Identify scope
exit separately from successful completion so a failed assertion is not
reported as a successful test. Preserve the existing operation sequence,
fixtures, page geometries, cache capacities, assertions and cleanup ownership.

Reuse the existing B-tree interoperability progress-timing mechanism through
one small shared test-support timeline. Do not copy its clock/state algorithm
into another helper. Retain the B-tree's existing geometry/phase fields.
Keep this support outside the ordinary production library; do not introduce
a second VFS, mock recovery path, system-wide tracing or runtime configuration.

This is diagnostic qualification work, not an asserted storage-algorithm
repair. If another timeout occurs, preserve its last entered/completed phase
and investigate that concrete operation before changing engine behavior.
No unchanged hosted retry, watchdog increase, skipped case, changed scheduler, additional
worker or successful-error fallback is authorized.

## Verification and delivery

Record the missing-progress observation against the original executable.
Verify the new ordered progress and scope-exit output against the real test,
including its existing database and journal assertions. Validate unchanged
CTest names, commands, labels, resource ownership and deadlines.

Run the complete fast tier and the affected native interoperability owners.
Use the actual change-aware plan for closing coverage and retain all required
exact-head hosted checks. The original 10-second pager gate and 30-second
B-tree gate remain. A successful diagnostic execution is not evidence that
the original timeout's cause has been diagnosed.

Review design and implementation before committing this single coherent
follow-up, append ADR-0106 ledger evidence, and squash only after exact-head
acceptance. Verify actual resulting main. Literal/name/unary/scalar-width and
IN work remain paused; this does not complete node 43 or admit a later node.
