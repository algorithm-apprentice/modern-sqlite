# ADR-0214: PR Quality Job Budget

- Status: Accepted
- Date: 2026-11-05
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source227 `8f5abc4`, ledger227, draft PR #294
- Related contracts: ADR-0009, ADR-0102/0106 and ADR-0211/0212/0213

## Observed job budget, not a test-performance failure

CI41940809279 attempt1 does not qualify source227. All three whole-TU
quality workers have explicit ten-minute job-limit annotations. Shard0 is
cancelled while processing the single lowerer target. Shard1 completes binder
but not its remaining targets. Shard2 completes its four analysis/compilation
targets in494 seconds, then formatting is cancelled. The legacy quality gate
correctly fails. Partitioning alone did not meet the declared job budgets.

The independent full-production quality bodies for sources225/226/227
complete in772,1499 and1000 seconds respectively, under their existing
thirty-minute job budget. Their setup takes42,79 and96 seconds. These runs
use the declared Intel profile and patched CMake4.4.5. They retain their own
source, compiler, image and execution identities; this is not a matched
hardware benchmark or a per-TU timing proof. The successful separate jobs do
not waive any failed standard-CI worker.

The Release interruption is distinct. Earlier completed Intel Release
builds at sources224/225/226 take121,64 and87 seconds. Source227's terminal
record instead retains an incomplete Build, an unexecuted Test and a
twenty-minute job-limit annotation; its archive has no Release entries.
Missing logs establish neither a particular compiler defect nor inadequate
normal Release capacity. Do not raise its budget on that evidence.

The user requested continued autonomous repair rather than another pause.
The next decision must explicitly distinguish infrastructure elapsed-time
budgets from test/model deadlines and measured engine-performance contracts.
Do not present the larger job budget as unchanged timing or an engine fix.

## Decision: one PR-only orchestration budget

Set `quality_shards.timeout-minutes` to20 for `pull_request` and retain10
for push. Use the existing event-expression mechanism directly; add no tool,
workflow dispatcher, evaluator, cache, provider or compiler wrapper.
The actual assignment, whole-TU CMake targets, one build process per worker,
three-worker maximum, shared-check owner and fail-closed gate stay unchanged.

This is an explicit bounded resource-policy correction, not an assertion that
twenty minutes is a measured upper bound. PR workers may now consume at most
sixty aggregate job-minutes rather than thirty. Standard hosted machines and
maximum concurrency do not change. A new overrun remains a failed gate, not
permission for automatic increases or unchanged-head retries.

This narrowly supersedes ADR-0211's preserved changed-quality job budget and
ADR-0213's ten-minute PR worker budget/no-increase decision. It does not
supersede ADR-0102's test assertions, inputs, allocation ceilings, benchmark
thresholds, isolated execution or test/model deadlines. In particular retain:

- ten minutes for the main quality worker and legacy result gate;
- twenty minutes for every Debug/Release and native-contract job;
- the unchanged Windows, sanitizer, full-quality, regression and fuzz budgets;
- five-second GoogleTest enumeration, individual CTest/model/child deadlines
  and the accepted native Python startup/body timing contract; and
- all checkers, warnings-as-errors, compiler work, cases, seeds and corpora.

ADR-0217 subsequently changes only the full-production quality job budget
from30 to45 minutes. It does not change this ADR's PR/main20/10 policy or
the other preserved job and test deadlines.

No `continue-on-error`, diagnostic filtering, skipped work, trust/permission
change, paid runner or repository-protection change is admitted. This policy
is not authorization to raise a failing test's timeout.

## Test-first and delivery

Review this narrow supersession before implementation. Update the existing
workflow-policy owner first so it rejects the old PR job budget, then change
the one workflow property. Keep the main/gate budgets, original platform
commands, complete shard coverage and non-success gate controls protected.
No new configuration parser or additional test framework is needed.

Run focused red/green, the complete native workflow-policy owner and complete
Linux tooling/shared selections. Use the actual source/batch verification
plans; reconcile unchanged C++ inputs, images, runtime definitions and original
complete fast/isolated/native/fuzz evidence rather than relabelling it as new
execution. No C++ or test-runtime change is part of this correction.

Admit one named source228 final-gate remediation in PR #294 under ADR-0106.
Record independent design, code and source acceptance before a normal commit
and ledger append. This does not admit another SQL slice or a general batch
exception. Preserve source227 and every earlier failure as failed evidence.

Require a fresh exact-head twelve-context standard CI plus the existing
four-job supplemental union. Every quality assignment must actually complete;
the new head's Release body must also pass under its unchanged twenty-minute
budget. No earlier successful Release run substitutes for that execution.
Integrated review, normal squash and actual-main ten-context qualification
remain mandatory before branch retirement. Node43, remaining SQL prerequisites
and IN are still incomplete.

## References

- [GitHub job timeout semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idtimeout-minutes)
- [GitHub expression-context availability](https://docs.github.com/en/actions/reference/workflows-and-actions/contexts#context-availability)
