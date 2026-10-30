# ADR-0177: Paired Native Storage and Write-Owner Diagnosis

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0106, ADR-0153, ADR-0175, ADR-0176

## Evidence and remaining uncertainty

The accepted ADR-0176 diagnostic source is commit `84b48f0`, tree
`1570f449aae0cfc53adb58fa9fa3e1e222701a6f`, in draft PR #274.
Its actual CI `41045743724` publishes every original model action and
verification. All 256 identities and 24 kinds match the local execution.
The model passes its unchanged 20-second owner in 2.20 seconds, recording
2,185,949 microseconds wall and 1,250,000 microseconds process CPU.
That pass does not explain or close actual-main's earlier 20.11-second
timeout.

The same native selection instead times out in the unchanged
`compatibility.write_session_sqlite` owner at 30.01 seconds. It reaches
2,202 of 2,225 declared results; the following 23 cases and two later tooling
controls are not execution evidence. This owner passes locally in 4.11
seconds. Eight other CI contexts pass; the PR's no-body contexts remain
distinct from executed checks.

The write compatibility executable runs 31 independently named cases in
lexical order. Its dependent differential trace and each crash scenario
remain indivisible. A local diagnostic translation of that original loop,
linked to the unchanged crash object and ordinary libraries, completes all
31 cases and observes 354 native database opens, 98 database syncs and
196 journal syncs. These are native-oracle counts, not Modern counts.
The observation does not identify which case or operation timed out on the
hosted runner.

The actual hosted model fixture is under the system TEMP on `C:`, while
RUNNER_TEMP is `D:\a\_temp`. GitHub documents RUNNER_TEMP as a job-owned
directory cleaned at job boundaries. Its runner-image announcement
`actions/runner-images#12744` records restoring the Windows `D:` workspace
after a performance regression. Neither fact proves that this job's
particular timeout came from storage, or that its two observed paths have
different performance.

## Decision: one bounded diagnostic continuation

Do not select a repair from those observations. Add one second, reviewed
diagnostic commit to the existing draft, retaining the normal four-commit
bound. It changes no engine behavior, test placement, workload, durability,
selection, process isolation, timeout, retry or failure policy.

Extract the existing model progress observer into one test-support owner
and reuse it for write compatibility. Preserve the model's existing record
prefix, phases and complete action traces. The shared observer retains
monotonic wall timing, actual Windows `GetProcessTimes` process CPU, an
explicit unavailable CPU field elsewhere, and explicit measurement/output
failure. Do not duplicate its clock or error policy in another helper.
Add before/after records for each of the 31 original compatibility cases,
and completion only after all existing assertions pass. Selected-case
commands retain their complete original selection. A phase record does
not divide a dependent trace or crash scenario into new test owners.

Before the one original native CTest selection, run a separate stdlib-only
paired storage control. Its two explicit roots are the unchanged TEMP and
RUNNER_TEMP values. Validate both existing roots before creating anything.
Each gets a freshly owned temporary child; never remove or mutate the
supplied roots or existing files.

The control performs 50 identical rounds per root, alternating which root
goes first. Each round creates one journal, writes and syncs a 512-byte
header, writes and syncs a 4,096-byte payload, then overwrites and syncs a
4,096-byte database page. Use real unbuffered files and `os.fsync`, not a
memory backend, skipped flush, `WriteThrough` substitution or injected
latency. Check complete writes and read back the exact journal/database
bytes before removing only that round's owned journal. Thus each root
executes 150 writes and 150 real syncs. Close handles and clean only the
owned temporary children on success or an ordinary error.

Publish bounded per-operation records with root identity, round, operation,
monotonic write/sync durations and real process CPU. Publish totals only
after all 150 writes, syncs and byte checks for that root complete. An I/O,
short-write, clock or publication failure fails explicitly. Do not choose
a volume automatically, add a timing pass threshold, rerun a model, or
interpret this small control as engine or crash qualification. Its Python
file-opening policy is not a claim of exact SQLite/Modern VFS flags.

Publish the captured progress for both original write owners from the
existing CTest log, including a failed correctness step. Preserve the
earlier-failure warning versus missing-success-log error distinction.
Do not execute either owner a second time to obtain output.

## Verification and admission boundary

Review this design before implementation. Preserve the executed green
original local model/compatibility baseline. Add focused test-first
controls for the paired population, alternating order, full byte checking,
invalid roots, short writes, real error propagation, output failure and
owned-child cleanup. Mocked syncs are unit evidence only; additionally run
the actual 300-sync pair locally and retain its complete output.

Require complete fast units, the complete unchanged native selection,
actual model trace identity and all 31 compatibility begin/end identities,
strict changed-source/header analysis, graph/discipline/format checks and
focused genuine sanitizer coverage of the shared observer and owners.
Ordinary library and pinned-oracle identities must remain unchanged.

After SourceAccepted, publish the normal second commit to the same draft.
Use actual hosted paired controls and owner phase/CPU records to document
and independently accept any subsequent correction. A green diagnostic
run alone is not a repaired deadline or a closed actual-main blocker.
Do not dispatch intermediate Full quality or blindly rerun failed heads.
Node 43, physical-repair qualification and later data-admission work remain
incomplete.

## External contracts

- [GitHub runner variables](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).
- [Windows workspace restoration](https://github.com/actions/runner-images/issues/12744).
- [Python filesystem sync](https://docs.python.org/3.13/library/os.html#os.fsync).
- [Python process CPU](https://docs.python.org/3.13/library/time.html#time.process_time_ns).
