# ADR-0181: Canonical Failed-Fixture Retirement

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0102, ADR-0106, ADR-0180

## Completed finding and owner inventory

Draft #275 has three accepted local sources through `0410dde`, tree
`56d0420`, ledger 117. Its final CI `41091130571` passes Native Windows,
sanitizer, Ubuntu Release, Linux native contracts and changed-source quality.
Both macOS configurations fail the new early-exit fixture control, not a
database assertion or execution timeout. Later unexecuted tests are not
qualification evidence, and no manual Full quality is dispatched.

The real fixture parent exits with status 17 before publishing readiness.
ADR-0180 then invokes the performance runner's group cleanup. Its initial
SIGTERM returns `EPERM` on the exited, unreaped macOS group. That exception
escapes before the parent is reaped. The readiness mechanism and original
deadline assertions otherwise run; the selected failed-setup owner is wrong.

A controlled local test of the unchanged source uses a real exiting parent
and the observed terminal-group errno. It reproduces the same wrapped error.
The diagnostic then reaps its known parent and closes its real streams.
This is not a request for different permissions.

Apple's public XNU `bsd/kern/kern_sig.c:1612-1621`, reviewed on 2026-10-30,
filters zombie members in this group path and can return POSIX `EPERM` when
no eligible member remains. The repository already maps this distinction:

- `tools/run_fast_tests.py:144-172` owns group-state inspection and
  `kill_owned_group`.
- It preserves the unreaped leader identity, attempts canonical retirement,
  and accepts terminal `EPERM` only after observing that same leader and
  exclusively zombie group members. A live member or missing leader remains
  an explicit error. It then reaps the owned parent.
- `tests/tools/fast_tests_test.py` already covers live-group rejection and
  zombie-only reaping after inspection.

Reuse that owner. Do not copy its process inspection, add another termination
algorithm, infer group death from a parent exit code alone, or catch and
discard a live-group error.

The constructor interposition also needs its proper scope. ADR-0180 patches
the shared `subprocess.Popen` attribute. Canonical group inspection invokes
real `ps` through that same module, so a global fixture constructor would
intercept unrelated inspection. Scope constructor replacement to the
performance module's binding, not the global standard-library module.

## Decision

For failed readiness only, use the existing `kill_owned_group` owner and retain
the current real-stream closure and failure propagation. Successful fixture
handoff still returns the same real process to the unchanged performance
runner, selector, original 0.2-second deadline and normal cleanup.

Use a standard-library namespace containing the original subprocess module
attributes and only the fixture constructor replacement. Temporarily bind
that namespace to `read_performance.subprocess`. Apply the same scoped binding
to the existing negative-control constructor observer. The real global
subprocess module and canonical process inspection remain untouched.

No engine, production Python tool, process-group helper, permission setting,
benchmark workload, capture limit, deadline, assertion or CLI contract changes.
The existing canonical helper's inspected terminal-state rule remains
authoritative; do not broaden it.

## Test-first and closure

First add an owning regression with a real parent exiting before readiness
and controlled terminal-group `EPERM`. It must reproduce the current wrapped
failure before changing the cleanup owner. Its own finally block retires
the known test parent even on the expected red result.

After correction, require real inspection of the unreaped parent's zombie
state, successful reaping, closed streams and explicit early-exit reporting.
Retain the existing live/no-readiness, delayed startup, original cold-start,
capture and descendant-disappearance assertions. Run the existing canonical
live-group rejection and zombie-state controls rather than invent another
definition of terminal ownership.

Require focused red/green, all performance-tool methods, the actual original
30-second CTest owner, complete fast units, graph/engineering checks and
independent implementation review. Preserve the failed macOS logs and honest
compiler/configuration evidence scope.

This is the fourth and final normal commit in the current bounded delivery
batch. All final exact-head hosted checks, the one actual manual Full quality
invocation and integrated review must restart against its resulting head.
Another significant finding requires a reviewed split, not a fifth progress
commit, an amendment, a blind rerun or a waived gate. No later feature or DAG
node begins here.
