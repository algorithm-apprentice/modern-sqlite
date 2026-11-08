# ADR-0236: Native Policy Owner Import Progress

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Blocked ancestor: PR #310, main `8a672321`, tree `16fd80c9`, ledger304
- Related contracts: ADR-0102/0106/0210/0227/0234/0235

## Observed failure and limits of the evidence

Original main CI42494743792 attempt1 fails Windows job128774522772 after
the native owning and complete correctness steps succeed. In the separate
six-owner tooling step, `tools.ci_workflow_policy` reaches its unchanged
ten-second end-to-end limit at10.03s without Python method output. The other
five tooling owners pass. Backend analysis after the tooling step is skipped.
The failed run, annotations and complete raw step body remain retained.

This is not the earlier167-versus170 population assertion, corrected by
source302. The same accepted source tree has successful premerge policy
execution. An earlier local invocation of this same owner separately timed
out at10.04s without output; its phase and cause were also not established.

The policy module imports `hashlib`, `tarfile`, unittest and mocking support.
Its first method reads the two workflow files. With the current non-verbose
command, no output distinguishes interpreter/module loading, discovery or
that first method. The protected read-compatibility owner's readiness and
thread samples on the same host belong to a different process and owner;
they cannot identify this failure's phase.

One separate local diagnostic uses the reported host versions, Python3.12.10
and CTest4.4.5, with import timing and verbose method progress inside the same
ten-second limit. All32 original native methods complete:31 unconditional
methods and the existing Windows-only publication control. CTest reports
5.5335s and outer wall time is5.5954s. Completed import output interleaves one
verbose method line; the complete original name set and unittest summary,
not only lines ending in `ok`, account for all32 methods.

The local control does not reproduce the hosted failure or prove matching
binaries, hardware, temporary paths or cache state. It uses no target-suite
warmup or module/provider replacement and does not change the configured
native owner. Tool versions and image hashes are read after the measured
execution. The source files had already been inspected; no cold-cache claim
is made. Its initial declaration-only path-normalization error is preserved
and did not execute the owner.

Automatic main Full quality42494743776 succeeds, but that status is not
replacement Windows evidence. No original main workflow has been replayed.
The preceding PR-only diagnostic authorization is exhausted and does not
authorize another request.

## Decision

On Windows only, add the existing standard Python `-X importtime` and
unittest `-v` diagnostics to the single canonical `tools.ci_workflow_policy`
registration. Keep the same configured interpreter, module selector and
repository working directory. Construct one conditional CMake argument list
before the existing registration and expand that list without empty entries;
do not introduce a wrapper, alternate interpreter, runner, sampler, clock,
cache, provider or additional process.

Retain `TIMEOUT 10` as the original combined end-to-end budget. Add no
`TIMEOUT_AFTER_MATCH` or readiness reset. No imports, discovery, methods,
fixtures or CLI children move outside CTest or before its clock. This is
observability, not ADR-0210's separate startup/body timing policy and not a
latency repair.

The same native32 and non-Windows31 methods, assertions, real child commands
and allocation/workload checks remain intact. Do not copy, split or omit
existing policy methods. The non-Windows command remains byte-for-byte
unchanged in the actual CTest argument list, with no empty optional arguments.
Preserve the policy owner's labels and processor reservation, the verification
owner's30s limit, and the four existing native startup owners'10s/two-slot
contracts and serial CI selection.

Import timing reports completed imports, not the current native stack or a
provider/OS cause. Verbose unittest reports method entry and completion, not
proof that a silent interval is a deadlock. A subsequent failure must retain
all available original output and remain failed. Logging has overhead; no
scheduling equivalence, speed improvement or fixed historical cause is claimed.
The existing output-on-failure tooling step already retains this output, so
no workflow change or extra job is required.

This narrowly supersedes ADR-0227's retained exact policy-owner command for
these two Windows diagnostic flags only. Its unchanged-owner budgets,
workloads, failure propagation and source-neutral-retry prohibition remain
binding. Every production C++ input is unchanged.

### Native-only unittest discovery boundary

- Amendment status: Accepted

The corrected argument-list implementation passes all six native tooling
owners, all32 native policy methods and the complete2325-case native unit
tier. The actual change-aware plan selects five tooling steps. Its Linux
all-tools unittest discovery then reports97 methods and eight subtest errors
from the four Windows process-metadata methods: their direct `pwsh` invocation
is unavailable in that host domain. Graph and engineering pass; the remaining
configuration/CMake-owner steps have not yet executed.

CMake already registers these four methods only under `WIN32`, as ADR-0227
requires, but direct unittest discovery has no corresponding applicability
guard. Mark only `NativePythonStartupObserverTest` as Windows-only using the
standard unittest platform decorator. Do not select by tool availability or
skip a supported Windows test when a dependency fails. Keep every native
method, command, assertion, deadline, resource reservation and cleanup control.

Run the complete Linux tool discovery again and report its four unsupported
platform skips explicitly, never as executions. Run the actual four native
owners with no skipped case under their unchanged limits. Retain the failed
Linux observation. Do not change permissions, install/substitute a shell to
evade the platform boundary, narrow the discovery pattern, or omit the changed
plan step. This only makes direct discovery honor the already accepted
Windows-only ownership; the policy owner's non-Windows31 methods remain active.

## Test-first and bounded descendant delivery

After independent design acceptance, update the existing real CMake
registration control before changing the registration. Record the missing
native diagnostic arguments as the focused red. Configure with Windows
enabled and disabled; verify the exact native argument list, unchanged
non-Windows list, original10s combined budget, absence of a match-based reset,
labels, directories and unchanged adjacent verification/startup owners.

Run the complete native policy owner and inspect its actual import/method
output under the unchanged deadline. Account for all original method names
despite interleaved diagnostics. Execute the existing six-owner native tooling
selection and applicable non-Windows policy/tooling checks. Use the actual
change-aware closing plan, complete fast-unit evidence and exact unchanged
C++ input/configuration/image reconciliation rather than invented new
compiler or timing executions.

Admit one named source305 corrective tooling slice descending from blocked
main `8a672321`, under ADR-0106. Keep a normal reviewable commit, independent
design/implementation/source acceptance and append-only ledger evidence.
This is not an open-ended CI-remediation batch or permission to start the
remaining SQL changes.

Require actual final-head local, standard and supplemental bodies before
independent integrated acceptance and normal squash. Actual descendant-main
qualification remains mandatory. Only separately reviewed proof of ancestry,
unchanged ancestor engine/owning tests, actual required descendant bodies,
all annotations and expected branch OIDs may close PR #310's delivery block
and retire its retained source reference alongside the corrective source.
The original main failure remains failed and causally unresolved unless new
evidence explains it. Preserve all three stashes. General VALUES sharing,
deferred/signed generation, preparation-error ordering, IN and node43
completion remain separate and unaccepted.
