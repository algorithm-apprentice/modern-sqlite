# ADR-0178: Job-Owned Native Runner Temporary Storage

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0106, ADR-0153, ADR-0175, ADR-0176, ADR-0177

## Completed diagnostic evidence

PR #274's second accepted diagnostic source is `7d77695`, tree
`3782052069fcdfc1b53c84c707104e7ea3fae242`. Actual CI `41053176786`
completes both real, unchanged-root storage controls. Each performs all
50 alternating rounds, 150 writes/syncs and 435,200 verified bytes, then
closes its files and removes its owned temporary child.

The original system TEMP is on `C:`; RUNNER_TEMP is `D:\a\_temp`. Actual
150-sync totals are 921,097,200 versus 50,434,300 nanoseconds. Mean sync
latency is 6.140648 versus 0.336229 milliseconds; medians are 6.2584 versus
0.3012 milliseconds. This is an 18.2633-fold difference in this matched
control, not an engine benchmark or proof of the earlier timeout's syscall.

The complete 256-action model passes its original 20-second owner in
2.90 seconds, recording 2,890,194 microseconds wall and 1,265,625
microseconds process CPU. The earlier actual-main model failure remains
unattributed.

Native instead fails `compatibility.btree_writer_sqlite` in 28.19 seconds:
all eight lanes reach the original common 25-second bound and are
explicitly retired/joined with status 31. Only eight of sixteen geometries
complete; the crash population is not completed. Completed native-oracle
geometry phases sum to 141,049 milliseconds across the eight concurrent
lanes. That sum is not whole-owner wall time. Both engines used the same
system temporary volume. Only 2,200/2,225 declared results execute; the
following 25 cases and two tooling controls are not evidence.

Eight other contexts are green. Actual case populations are four complete
2,235-case runtime selections and the 55-case Linux benchmark selection.
The two PR no-body contexts are not executed workload evidence.

## Ownership and native mechanism inventory

The write model, write compatibility and canonical Windows temporary
directory obtain the process's standard filesystem temporary root. The
B-tree children borrow their parent's directory under that same inherited
root. No engine-specific path policy is needed.

Modern's anonymous Windows VFS path uses `GetTempPathW` in
`src/platform/windows_vfs.cpp:398-423`, before its existing unique naming
and native open path. The pinned SQLite `winGetTempname` likewise uses
`osGetTempPathW` in `src/os_win.c:4174-4202`; `winOpen` admits that name at
4367-4377. Neither engine's VFS or file flags change here.

Microsoft documents TMP before TEMP in `GetTempPathW`'s search order and
requires the caller to validate existence/access. GitHub documents
RUNNER_TEMP as job-owned storage cleaned at job boundaries, subject to the
runner user's deletion permissions. Its environment file propagates
variables to subsequent steps, not retroactively to the writing step.

## Decision

Declare the Native job's temporary-storage ownership explicitly rather
than inherit a different system volume for its file-backed qualification.
Use one freshly created, unique directory beneath the existing RUNNER_TEMP
for both TMP and TEMP throughout subsequent Native job steps. Resolve the
root from the runner variable; do not encode `D:`, choose a volume from
measurements, substitute a memory backend or change other jobs.

Before checkout/toolchain/test steps, validate the original TEMP,
RUNNER_TEMP and environment-file destination as appropriate absolute
existing filesystem locations. Reject missing/invalid roots explicitly.
Create a new ASCII GUID-named child without overwriting an existing
directory. Publish its identity, preserve the original TEMP in a dedicated
job variable, and append UTF-8 environment-file bindings for the selected
root, TMP and TEMP. Do not change machine/user environment settings or
RUNNER_TEMP itself. The runner owns final job-root cleanup; fixture/process
owners retain their existing narrower cleanup responsibilities.

Keep ADR-0177's paired control on the saved original TEMP and the unchanged
RUNNER_TEMP. It must still measure both original roots, not accidentally
compare the selected root with itself. Publish saved/original and selected
roots alongside the existing native owner records.

This supersedes only ADR-0176/0177's diagnostic-only prohibition on a
temporary-placement change. All workloads, real filesystem operations,
durability calls, engine/pinned-source code, flags, stack defaults,
assertions, crash cuts, 256 dependent actions, 31 compatibility cases,
eight B-tree lanes, common 25-second child bound, original 20/30-second
owners and the complete CTest command/selection remain unchanged. Do not
add retries, ignored failures, additional owner executions, new timing
allowances or unavailable-root fallbacks.

This is a bounded qualification-environment correction, not a claim that
every historical delay was caused by the observed volume difference.
Arbitrary host saturation may still fail the original deadlines.

## Test-first qualification and delivery

Before implementation require independent design acceptance. Retain the
actual failed hosted run and matched controls. Add a focused policy red
for missing job-root ownership. Also demonstrate that the actual captured
prechange native fixture roots do not satisfy the proposed runner-owned
root requirement; do not manufacture a database assertion failure.

Execute the actual initialization script locally, including invalid-root
and environment-file failure controls. Prove a unique child is created,
both root bindings are propagated to a subsequent real process, original
TEMP remains available to the paired control, and unrelated files/roots
remain intact. Local runner-root surrogates do not prove cloud performance.

Run complete fast units and the whole unchanged Native selection under
the selected local job root; authenticate all model, B-tree and write
compatibility fixture paths and original complete work. Run the changed
workflow/tool controls and actual paired control with the new bindings.
Reuse successful Linux/sanitizer/format evidence only after reconciling
its unchanged code, binaries, configuration, environment and selection;
do not repeat or reuse mismatched evidence.

Require SourceAccepted, ledger evidence and a normal third commit to the
same draft before final exact-head gates. Require all actual hosted bodies,
one required final manual Full quality invocation, integrated review,
guarded squash, actual-main qualification and delivered-ref cleanup.
No data-admission successor or node completion is admitted before those
gates. Preserve unrelated draft #269.

## Primary contracts

- [GitHub runner variables](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).
- [GitHub environment-file propagation](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-commands#setting-an-environment-variable).
- [Windows temporary-path selection](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppathw).
