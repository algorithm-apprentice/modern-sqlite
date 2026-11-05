# ADR-0217: Full-Production Analysis Job Budget

- Status: Accepted
- Date: 2026-11-05
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source236 `0e3a0368`, draft PR #296
- Related contracts: ADR-0009, ADR-0102/0106 and ADR-0211/0212/0213/0214

## Completed failure diagnosis

Source236's local full34 and standard CI42095818858 pass. The latter has
all12 contexts and authenticated actual bodies, including Windows19/2293/2
and39 native Python methods in3.25 functional seconds. This is not a new
Windows test failure.

Supplemental42095816071 attempt1 does not qualify the source. Its platform
regression and both fuzz-family jobs succeed, but `full-quality` is cancelled
at the declared thirty-minute whole-job limit. Its build step runs1647
seconds; setup and other work consume the rest of the1824-second recorded
job interval. Formatting is skipped after cancellation.

The completed archive is available and valid. Its build log records43 of46
Ninja actions:42 production object compilations and the instrumentation
library link. Binder, bytecode, lowering and optimizer all have completed
object events. There is no compiler/analyzer diagnostic in the preserved
build body; its only error line is cancellation.

Comparison with the same-platform configured production target set identifies
the two objects without completion events:

- `src/temporary_storage/temporary_storage.cpp`
- `src/vm/vm.cpp`

The ordinary library link also has no completion event. Do not call these
missing completions successful or infer a particular compiler stall, host
contention mechanism or performance regression from the log.

The preceding source232 full-production body succeeds in1349 seconds within
a1467-second job. Its later same-tree automatic main attempt separately
exceeds30 minutes but has no retained build body. Older accepted full bodies
take772,1499 and1000 seconds under the same policy. Those observations keep
their own source, compiler, host and execution identities; they are not a
matched timing experiment.

The current failure demonstrates sustained work reaching the job budget,
not a failing functional test or a broken quality gate. The failed attempt,
its annotations and every partial artifact remain failed evidence. No
unchanged-head workflow retry was requested.

## Decision: one bounded orchestration-policy change

Set only `jobs.full-quality.timeout-minutes` in `full-quality.yml` to45.
Retain the same standard Intel runner, verified CMake profile, analyzer,
compiler, checker configuration, three build processes, both production
library targets and final formatting step. No source selector, sharding API,
new provider, compiler wrapper, cache or executor is added.

This increases the full-quality job budget by15 job-minutes, or50 percent,
per invocation. It is an explicit resource-policy correction, not a claim
that45 minutes is a measured upper bound. A new overrun remains a failed gate,
not permission for another automatic increase or retry.

Whole-production sharding would add runners, target-assignment/coverage
machinery and another aggregate gate. That larger change is unnecessary for
this bounded failure: the unchanged operation sequence is making progress,
and every selected changed-TU worker completed under its already accepted
budget. Do not replace the proven full-production command as an incidental
timeout repair.

This narrowly supersedes ADR-0211/0214's retained thirty-minute full-quality
budget. It does not alter:

- PR changed-quality workers20m, main worker/result gate10m;
- runtime and native-contract jobs20m, Windows, sanitizer, regression or fuzz
  job budgets;
- any test, model, enumeration, startup/body, child-process or per-input fuzz
  deadline;
- assertions, checker rules, warning policy, workloads, allocation limits,
  corpus sizes, seeds or benchmark thresholds; or
- runner trust, permissions, repository protection or fail-closed behavior.

No `continue-on-error`, skipped analysis, filtered diagnostics, paid runner,
security bypass or success-shaped fallback is admitted. Every production
target and formatting must actually finish on a fresh accepted head.

## Test-first, reconciliation and delivery

Review this policy before changing the workflow. Update the existing
workflow-policy test's exact full-quality budget assertion first, record its
failure against30, then change the one workflow property. Keep the existing
runtime/native20m and PR/main20/10 assertions and command checks intact.

Run focused red/green, the complete native policy owner and the complete
selected Linux tooling/shared checks. Generate the actual source and final
batch plans. Reconcile unchanged source236 C++ inputs, compiler/configuration,
images, definitions, environment, process modes, native contracts and original
four fuzz campaigns before reusing them. Fresh tooling/graph checks must
cover the changed workflow, test and documentation. Do not repeat matching
successful C++ work or relabel historical execution as a new run.

Admit one named source237 final-gate remediation after source236 in PR #296
under ADR-0106. Keep both normal commits, independent design/code/source
acceptance and append-only ledger evidence. This does not start another SQL
slice or authorize an open-ended CI-remediation batch.

Require fresh exact-head standard12 plus all four actual supplemental
bodies. The old cancelled attempt cannot satisfy this union. Independent
integrated acceptance precedes normal squash, actual-main10 qualification
and expected-OID source retirement. Remaining generation, IN and node43
completion remain unaccepted.

## Reference

- [GitHub whole-job timeout semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idtimeout-minutes)
