# ADR-0176: Native Stateful Model Deadline Diagnosis

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0106, ADR-0153, ADR-0174, ADR-0175

## Actual-main blocker

PR #273 passes its exact-head nine-context CI, all four actual manual Full
quality jobs and integrated review before normal squash delivery as
`5d8798c`, tree `9c917fd2caf73dbf4e11fb55f7b5ce3f6529c7a6`.
The delivered branch is retired only after exact-tip/content proof.

Actual-main CI `41035893096` passes eight contexts, including all five
2,235-case non-Windows runtime domains and both 55-case benchmark domains.
Fresh actual-main production quality also passes. Native instead times out
in `WriteSessionModel.MatchesDeterministicMixedActions` at 20.11 seconds
under its original 20-second limit. Only 2,045 of 2,225 declared Native
results are reached. The following 180 cases and two later tooling controls
are not execution evidence. Premerge success on the same tree does not
close that failure, and the failed workflow is not blindly rerun.

The preserved native owner passes locally in 1.07 seconds. Earlier actual
hosted observations are 1.86, 8.27, 5.85 and 14.85 seconds. These establish
variability, not the responsible operating-system operation or engine path.

## Completed local inventory

The owner contains one dependent model sequence, not independent trials.
It retains SplitMix64-v1 seed `0x4d53514c57524954`, 256 total actions including
its fixed prelude, all 24 action kinds, the 128-row/eight-savepoint bounds,
per-action result/finalize/catalog/row/type/payload/counter/autocommit checks,
reopens and final rollback. Its next action depends on the evolving
reference state. Do not copy the independent B-tree process lanes into this
sequence or replace it with independently permitted deadline shards.

A diagnostic-only copy of that complete owner delegates through the
unchanged native VFS and ordinary library. All original assertions pass.
It observes 1,562 database reads, 136 database writes, 50 database syncs,
1,637 database locks, 1,587 unlocks, 246 journal writes, 100 journal syncs
and 11,697 delegated calls. Its zero-delay observation is approximately
1.09 seconds wall, 1.06 seconds process CPU and 0.08 seconds in sync calls.
Those are diagnostic observations, not qualifying engine timings.

Adding 130 milliseconds before each real delegated sync reaches the same
20-second bound. The 150 original sync calls alone contribute 19.5 seconds
at that artificial delay. This proves sensitivity, not that hosted sync
latency caused the actual failure. No injected-delay source enters the
ordinary model or production library.

The original model reports its rich deterministic trace only after an
assertion fails. A deadline termination instead exposes only the GoogleTest
startup text. The actual failed action, execution/verification phase and
process-CPU contribution are currently unobserved.

## Decision: bounded observation before selecting a repair

First deliver one localized diagnostic source in a new ordered pull
request, not a fifth commit in merged #273.

Retain the complete existing owner, command, labels, 20-second deadline,
seed, action population, SQL, transaction boundaries, physical durability,
per-action checks and final cleanup. Do not increase limits, batch
autocommits, omit verification, substitute a memory backend, copy fixtures,
run only part of the sequence or declare a local pass to be a hosted fix.

Add model-owned progress records at initialization, before each action,
between action execution and state verification, after verification and at
completion. Reuse the existing action identity/name and generator metadata;
do not create another generator, reference state or execution algorithm.
Records contain bounded indices/phase identity and monotonic wall elapsed
time. On Windows also record actual process CPU from `GetProcessTimes`,
with a run-local baseline. Do not call the Windows C-runtime `clock` value
process CPU. Other hosts explicitly identify unavailable native CPU data
rather than publish zero as a measurement.

Keep the observer in the test owner, outside the ordinary library and
benchmark timing. A failed clock query or diagnostic write fails explicitly.
Do not silently replace unavailable measurements with success-shaped data.
Record the owned fixture path before database work. Do not change temporary
placement or infer that one volume is faster merely from its name.

The native workflow retains its exact original correctness command and
failure semantics. After an attempted selection, publish the model's
captured progress from CTest's existing `LastTest.log`, including on a failed
test step. Do not add a second model execution, a retry, `continue-on-error`,
an ignored exit code or a different selection. Report explicitly when
earlier build/test failure prevented the owner or its log from existing.
Record the runner's existing TEMP/TMP/RUNNER_TEMP values without changing
them during this diagnostic source.

## Qualification and continuation

This source is a behavior-preserving diagnostic addition. Start from the
executed green local original 20-second owner; an artificial failing model
assertion is neither necessary nor permitted.

Before commit require independent design and implementation review,
complete fast units, the complete original model, unchanged graph and
engineering checks, strict changed-source analysis and pinned formatting.
Machine-check the diagnostic population against all 256 original action
identities and all 24 kinds, preserving every action/verification pair and
the terminal coverage check. Retain the untouched source and executable,
the original actual-main failure and the diagnostic-only delegated-call
manifest. Run selected native, sanitizer and workflow-policy checks without
claiming a passing local model repairs the hosted deadline.

Publish the accepted diagnostic source as a draft. Use its actual native
phase/CPU/path evidence to select a separately documented, independently
accepted correction. If evidence remains insufficient, report that plainly;
do not turn timing speculation into a repair or weaken the contract.
Final gates and actual-main proof remain required for any completed repair.
The Native model, physical-repair qualification, later data-admission
correction, node 43 and the project remain incomplete until their own gates
close.
