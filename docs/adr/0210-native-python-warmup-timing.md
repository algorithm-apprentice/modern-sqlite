# ADR-0210: Native Python Warmup Timing

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #293, main `aa7eb8b`, ledger221
- Related contracts: ADR-0102/0106, ADR-0186, ADR-0194/0195/0197/0209

## Context and explicit timing-contract change

The user requests that required modules load before functional-test timing
starts. The existing native `differential.read_compatibility_tool` already
imports its standard-library and harness dependencies in the same process
before unittest enters `setUpModule`. Its flushed `module-ready` record
identifies that boundary before the first test or fixture executes.

The old ten-second CTest deadline includes interpreter startup, imports,
discovery and the entire suite. This decision explicitly replaces that
combined budget for this one Windows owner; it is not an optimization that
preserves the old end-to-end limit or proof of the historical stall's cause.
The failed observations remain failed, and ADR-0208 remains exhausted.

## Decision

Reuse CTest's `TIMEOUT_AFTER_MATCH` and the existing owner readiness record.
Keep `TIMEOUT 10` as the startup/import/discovery budget. On Windows only,
give the same process ten further seconds after its first matching
`read-compatibility-progress` record with `test:null` and
`phase:"module-ready"`. No test body or fixture moves before that marker.
The total bounded allowance is therefore up to twenty seconds, not ten.

CTest changes the deadline once and clears its matchers. A subsequent
readiness record, including the existing watchdog lifecycle test's record,
cannot restart the functional deadline. The readiness pattern must not
match `bootstrap-ready`, `harness-ready` or a test-specific record.
Before readiness, a stalled import still times out. After readiness, a
stalled suite still times out. Import and test exceptions retain their
nonzero status; readiness is not a success or skip expression.

Use the existing same-process imports as warmup. Do not add a separate
Python process, pre-read files, invoke the suite twice, replace modules,
change providers or introduce another runner, cache, timer or watchdog.
Preserve the interpreter, `-X importtime`, `-m unittest -v`, import order,
working directory, two processor slots, all methods and real CLI children.
The eight-second traceback timer and one-second bounded-child controls
remain inside the functional phase. Child process startup remains test work.
All non-Windows registrations and every other owner's deadlines are unchanged.

Retain all import, operation/reporter and lifecycle diagnostics. CTest's
per-test execution duration excludes the interval preceding the match;
do not compare that number with historical whole-process durations as a
speedup. The existing module-entry and Unix timestamps remain available
for attribution, and outer wall time must be recorded for timing controls.

This narrowly supersedes the combined-owner timing and no-off-clock-import
clauses of the related diagnostic ADRs. It does not relax assertions,
workloads, child limits, security controls or unrelated deadlines, and does
not reopen the exhausted source-neutral requalification exception.

## Validation and delivery

Extend the existing CMake integration control before changing registration.
Configure its actual declarations with Windows enabled and disabled.
Require the new native timeout and exact readiness expression in CTest JSON,
the unchanged startup timeout and resources, and no change to the adjacent
CLI owner or non-Windows command. Keep the existing actual-import/no-suite/
no-watchdog control and verify readiness precedes watchdog arming.

Use owned, real-CTest timing controls to show that a delayed startup followed
by bounded work passes only with the phased contract. Separately retain
startup/missing-marker timeout, post-readiness timeout, repeated-marker
timeout and pre/post-readiness nonzero-exit controls. Diagnostic delays
belong only to those fixtures, never to the shipped owner.

Run the complete native Python and workflow-policy owners, non-Windows
owning checks and selected graph/discipline/tooling checks. Reconcile
unchanged C++ sources, images, configurations and complete fast evidence;
never claim new execution from reuse. Preserve the actual new runtime
property in discovery/reconciliation. Obtain independent source review,
record the normal source and ledger, and use the normal exact-head
hosted/integrated/main delivery gates. Node43 and remaining SQL prerequisites
are not completed by this change.

## References

- [CTest TIMEOUT_AFTER_MATCH](https://cmake.org/cmake/help/v4.3/prop_test/TIMEOUT_AFTER_MATCH.html)
- [CTest 4.4.5 single-match implementation](https://github.com/Kitware/CMake/blob/v4.4.5/Source/CTest/cmCTestRunTest.cxx#L78-L97)
