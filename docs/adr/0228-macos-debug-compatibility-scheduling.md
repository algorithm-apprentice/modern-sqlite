# ADR-0228: Reserved Two-Slot macOS Debug Compatibility

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source277, PR #305, head `105eb7b`
- Related contracts: ADR-0102/0106/0201/0227

## Observed qualification failure

Original CI42380920331 attempt1 fails only macOS26 Intel Debug
job128428160511. Its `differential.read_compatibility` owner reaches60.19s
under its unchanged60s deadline.2340 distinct cases pass, this owner fails,
and ten of2351 declared cases remain unexecuted, including the protected
lowering/optimizer OOM owners. The concurrent subquery owner completes59.30s.
No owning assertion diagnostic or native wait stack is captured; neither a
deadlock nor an OS/provider cause is established.

The retained PR #304 premerge control runs the same engine, corpus, verifier,
macOS job definition and scheduling policy: read31.57s/subquery28.05s.
Both bodies report macos-26 image20260824.0517.1 and AppleClang21.0.0.21000101.
Those matching observations do not make separate hosts/caches/schedules
equivalent. The failed original run remains failed and is not retried.

Windows in the new run actually passes19 owning/2341 correctness/6 tooling
cases and all39 original Python methods. It supplies three genuine startup
samples before readiness1.7366192s. Those positive observations neither
qualify the failed macOS context nor explain PR #304's historical Windows
stall. Original source277 executions remain frozen until completion or the
accepted interrupted-prefix disposition below.

## Existing mechanism and bounded scope

The existing seven CMake registrations share one options/properties owner.
Only ASan currently selects `--jobs 2` and `PROCESSORS 2`. Non-ASan macOS
Debug therefore runs each corpus serially while CTest's two slots may
concurrently host two different corpora. Each of those long-lived owners
admits only one child at a time under its own unchanged wall deadline.

Reuse ADR-0201's already implemented verifier scheduler. It admits at most
two futures, consumes corpus order, stops admission on the first ordered
failure, cancels and joins on close, and invokes the same `_run_bounded`
process/pipe/deadline/cleanup owner for every trace. The single-slot default
and regeneration stay unchanged. No new scheduler, runner, batch protocol,
process reuse or engine mechanism is added.

The read corpus contains512 cases (507 comparisons/5 boundaries); subquery
contains483 (481/2). The remaining five contain55,74,54,98 and85 cases.
Every owner also retains both its native-valid unsupported-prepare and
ordinary-prepare-error controls. Preserve the512 cap, complete case lists,
all operations, independent runner processes,10s child deadlines and every
capture limit. No generator/profile/oracle content changes are needed.

## Decision

Extend only the shared CMake admission predicate to ASan OR the existing
single-configuration `APPLE` Debug profile. All seven native-differential
owners in that macOS Debug profile select the existing `--jobs 2` and reserve
`PROCESSORS 2`. Under the unchanged CI `--parallel 2` envelope, one such owner
uses the two admitted child slots, rather than running two serial owners
concurrently. Do not add a CTest worker or increase host/job resources.

Every60s owner deadline and10s child deadline remains unchanged. Existing
ASan two-slot/`RUN_SERIAL` behavior remains unchanged. Do not add `RUN_SERIAL`
to ordinary macOS Debug, alter protected OOM owners, change macOS Release,
non-Apple Debug/Release, native Windows or other test registrations.
This is bounded scheduling of unchanged verification work, not an engine
speedup or proof of the host slowdown's cause.

## Evidence and delivery

Before editing the selector, extend the existing workflow-policy owner with
actual CMake/CTest declaration controls. Exercise Apple Debug/Release and
non-Apple Debug/Release with and without ASan; the unchanged policy must
genuinely fail the Apple Debug jobs/resource expectation. Require all seven
commands, case/oracle/runner arguments,60s deadlines and original serial
properties. Retain the Python tool owner's separate `PROCESSORS 2` and10+10
Windows timing contract.

Use the current-source trace images whose build/input identities are
authenticated, not a stale cached image. After source277's active closing
is completed or explicitly disposed below, retain a bounded local serial/
two-slot control over the same read and subquery corpus work and unchanged
images. State its actual platform; Linux/Windows measurements do not
reproduce or qualify the macOS host. Run all seven real two-slot owners and
the existing verifier cancellation, ordering/boundary/error controls;
reconcile unchanged engine/fast evidence. Current macOS acceptance requires
actual new-head hosted execution.

The recorded current-image Linux/GNU13.3 Debug replay uses one parent
environment for both retained declarations. Individual read/subquery owners
take12.8466s/9.69895s with the serial-per-owner arrangement and7.50785s/5.82946s
with reserved two-slot owners. Total pair wall time changes12.8958s->13.3808s:
this is not an overall speedup. All seven two-slot local owners pass their
original bounds with complete work and both error controls. Earlier separate
invocations had different full environment hashes and remain separate
observations, not the matched comparison. None of these Linux results
reproduces or qualifies the macOS host.

After independent design/source acceptance, commit one coherent source278
normally. Accepted source277 and this related qualification correction form
one ordered two-source PR #305 batch under ADR-0106; source277 remains
unchanged. Do not push while its original supplemental run is active or
cancel it. Preserve its failed CI and all successful bodies under their
original identities.

Replan and close the final exact batch head. Reconcile genuinely unchanged
local domains through ADR-0102; do not claim historical work as new execution
or reuse mismatched configuration/environment. Required standard/supplemental,
integrated/squash/main gates remain. Only separately accepted qualified-
descendant evidence can close PR #304 and retire old/new source refs at their
expected OIDs. Keep all stashes. General generation, IN and later DAG nodes
remain blocked.

## Accepted interrupted-prefix disposition

The independent ordering amendment narrowly replaces the original requirement
to finish an obsolete local fuzz suffix after source277's standard CI failed.
Its completed seven configurations/builds, quality2objects/1link, shared35/167,
GNU Debug2332, Release2332, ASan2324, instrumentation2353 and58 serial native
contracts remain authenticated at the original source.

After amendment acceptance, the parent verified the exact owned local process
identities and stopped only its active read-SQL fuzz leaf. The original
supervisor chain exited and termination was verified before repository
mutation. LibFuzzer reported interruption and exit72; its last progress record
was7906, not completion of20000. The raw log and partial mutation inputs remain
retained. No full38 completion or fuzz-campaign credit is claimed. Both
original hosted attempts had already completed; none was cancelled or retried.

This permits the accepted scheduling correction to proceed after an honest
prefix disposition. It does not reduce a corpus, workload, assertion, seed,
deadline, security control or final gate. Incomplete fuzz work is never
reusable qualification and is not resumed at a reduced remaining-run count.
The complete final four20k/seed102 campaigns remain required; when executed
locally, they use fresh owned writable directories and unchanged canonical
seeds. Only fully completed, exactly input/environment-matched domains may be
reconciled under ADR-0102.
