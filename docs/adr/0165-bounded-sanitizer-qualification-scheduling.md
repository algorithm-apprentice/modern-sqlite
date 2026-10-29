# ADR-0165: Bounded Sanitizer Qualification Scheduling

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Related decisions: ADR-0102, ADR-0106, ADR-0153, ADR-0164
- Design and named delivery exception accepted before implementation at proposal `7ee4f284`.

## Measured final-head result

PR #268 head `bdd5688`, tree `0ed52edb`, passes every one of the
2,187 unique isolated sanitizer cases. All six explicitly outstanding local
cases actually pass on hosted native Linux under their original limits.
The exact-head manual Full quality run also passes all four jobs, including
ordinary/instrumented quality, GNU runtime/shared checks and all four original
20,000-run campaigns.

The sanitizer job is nevertheless cancelled at 15m03s under its unchanged
15-minute envelope. Its build takes 287 seconds and its successful test step
takes 587 seconds. Setup and completion leave effectively no scheduling margin.
Do not count the cancelled job as successful or retry unchanged code merely
to obtain a warmer cache.

The ordinary GoogleTest discovery currently applies `RUN_SERIAL TRUE` to
every case under ASan. This was the conservative ADR-0153 resource baseline.
The actual hosted ReadLowering family has 176 cases, 36.76 total case seconds
and a 7.96-second maximum. It contains the measured aggregate watchdog that
originally required uncontended execution. Other ordinary cases remain
separate processes but unnecessarily reserve the entire job while starting
and completing individually.

## Decision

Retain an uncontended ASan domain for the complete `ReadLowering*.*` family.
Discover that family through the existing GoogleTest/CMake interface and the
existing sanitizer serial-property list. Discover its exact complement from
the same executable with the complementary GoogleTest filter, without that
serial property. Use one canonical family filter, not a copied case inventory,
another binary, a new launcher or a batching framework.

Non-ASan configurations retain their original unfiltered single discovery.
Every case retains its own CTest process, command, name, labels, timeout and
assertions. The existing four combined OOM partitions, preparation consumers,
native differential domains, resource locks and optimizer `PROCESSORS 2`
ownership are unchanged. Existing CTest `--parallel 2` bounds concurrency;
no additional worker or CPU allocation is introduced.

This supersedes only ADR-0153's blanket ordinary-unit serial registration.
It does not supersede the protected lowering family's resource guarantee,
any workload, failure prefix, allocation bound, deadline, sanitizer option or
the 900-second whole-job limit.

## Verification before delivery

Add a focused policy red before changing discovery. Compare actual complete
CTest JSON before/after: normal configurations must be identical; ASan must
have the same unique name union and execution contracts, with only the
intentional serial-property removal on the complementary ordinary cases.
No missing, duplicated, disabled or differently commanded case is permitted.
Both installed CMake versions support `TEST_FILTER` and multiple discoveries
of one target; verify the partition against the actual GoogleTest names.

Run complete fast units, the protected lowering family, all owning resource/
allocation controls and the complete isolated sanitizer selection under the
same two-slot scheduler. If contention causes any original deadline to fail,
resolve that evidence before delivery rather than relaxing the deadline.
Final gates run against the exact new head, including the original 900-second
job envelope and all hosted requirements preserved by ADR-0164.

## Local qualification boundary

The complete two-slot local sanitizer selection executes all 2,187 unique
cases: all 2,117 isolated units pass, including the complete protected lowering
family and allocation owners. The six ADR-0164 local limits remain failures.
An additional unchanged `performance.write_performance_tool` command times
out at its original 30 seconds before any ordinary unit starts. It overlaps
only two other unchanged performance-tool registrations, not the newly
parallel ordinary cases.

That tool's complete 26-test diagnostic finishes successfully in 32.19 seconds
wall time, with 2.22 user and 2.34 system seconds. Its synthetic baseline uses
`TemporaryDirectory(dir=ROOT / "build")` on the mounted source filesystem and
preserves all 108 timing and 36 diagnostic records. Native Windows passes the
original complete 30-second CTest owner in 8.29 seconds; the preceding hosted
sanitizer head passed it in 1.93 seconds. None replaces the failed local gate.

Accepted evidence placement: preserve the complete local result as 2,180
passes and seven failures, not a full sanitizer pass. Require all seven named
cases to execute and pass on the final exact hosted head under their original
deadlines, in addition to the successful whole-job envelope. No tool code,
temporary-file ownership, source-identity work or limit is changed.
This additional boundary was independently accepted before final delivery.

## Named fifth-commit delivery exception

Authorize exactly one additional, reviewable scheduling-remediation commit
after PR #268's four accepted commits. This named five-commit exception under
ADR-0106 is limited to this measured final-job qualification closure.
Production C++ and the published prefix remain unchanged. The fifth commit
requires accepted design, focused red/green, complete fast evidence,
implementation review and append-only ledger evidence before publication.
No sixth commit, new feature admission or later DAG node is authorized.

All required exact-head checks and integrated acceptance remain prerequisites
to the user's requested squash merge and actual-main verification.
