# ADR-0208: Bounded Native Import Requalification

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Scope: PR #293, accepted sources215/216 only
- Related contracts: ADR-0102, ADR-0106 and ADR-0194/0195/0197

## Observed failure and unchanged inputs

Sources215/216 are independently accepted at head
`cdc123a222c6d0ecdfc71f114ac8be5edfa6cc88`, tree
`37726cdfe2e397ff7735c670681e967c40bc69b2`. The combined code/local integration
review finds no unresolved issue, and the complete final-tree local
verification is recorded. This is not final delivery acceptance.

CI41769494913 attempt1 fails native Windows job126375598026. The declared
correctness selection executes2272 passing cases, including all2253 native
units, before `differential.read_compatibility_tool` exceeds its original10s
deadline at10.12s. The following15 correctness cases and the separate native
tooling/backend-quality steps are unexecuted. This is an executed timeout,
not a runner-acquisition failure or passing evidence.

Existing progress records locate7.0920203s between test-module entry and
harness readiness, with203.125ms reported owner CPU. The profiled `_hashlib`
creation and execution intervals are3.103865s and1.0130094s; CPython importtime
reports4.117581s self time for that extension. These boundaries include
observer/reporting effects. They do not identify a particular DLL, provider,
security product, filesystem, scheduler or OS cause.

Thirty-seven method-finished records follow. The final Windows fixture
control reaches `cli-start` before CTest kills the owner. No assertion failure
is observed, but its unfinished body is not qualified.

The accepted-source prefix CI41750299668, Windows job126313441914, contains an
actual4.35s passing occurrence of the same owner with Python3.12.10. The
complete `tools` subtree, owning test, fixtures, CMake/presets and CI
registration are unchanged between sources215 and216. The matching version
and source do not establish matching host, interpreter-binary or cache
conditions. That prefix is diagnostic control only.

The other eight exact-head CI contexts complete successfully, with the two
intentional PR body skips explicitly nonexecuted. Supplemental41769714056
attempt1 completes its four actual platform/runtime/fuzz/quality domains.
Their source and original attempt identities remain recorded; neither
replaces the failed Windows owner.

## Decision

### One named exception, not a new default retry policy

For this failure only, narrowly supersede ADR-0106 section4's requirement
that the next qualification cycle follow an implementation correction.
Permit one additional, source-neutral premerge qualification cycle after
this decision is independently accepted and committed normally.

The enabling commit may change only this ADR and its index entry. Preserve
every production, test, fixture, build, verification-tool and workflow input
from accepted head `cdc123a`. Verify the exact documentation-only delta,
append its predecessor-linked ledger record and keep PR #293 draft.
Do not invent a code fix, a diagnostic change, an empty commit or a claim
that import latency has been repaired.

Publication creates a new final documentation-bearing head. Pin that exact
head and its actual CI run before counting any result. Do not rerun the
failed original job/run, rewrite history or overwrite its failure evidence.
The enabling commit consumes this single additional qualification cycle;
another policy-only commit may not reset the allowance.

### No test, environment or evidence relaxation

The new final head must genuinely pass all nine required CI contexts and
the existing supplemental platform-regression, read/write fuzz and full
production-quality domains. Authenticate actual bodies, checkout trees,
populations and attempts. Preserve the original Windows19 owning cases,
2288 correctness cases, two tooling cases, backend analysis, and every
original assertion, child limit and workload.

Keep the same interpreter-selection rule, command, flags, working-directory
contract, runner family and10/8/1 owner/traceback/child limits. Do not add
prewarming, a different interpreter or crypto provider, security exclusions,
new worker/shard arrangements, skipped cases, extra time or provider
substitution. Record actual host provenance without claiming that separate
runner instances are identical.

Reconcile unchanged local source, configuration, images, selections and
execution identities under ADR-0102 instead of replaying matching successful
local work for a documentation-only change. Preserve original execution
trees and every prior failure. A fresh plan alone is not execution evidence.
Prior hosted results retain their original heads/attempts and do not satisfy
the required new-final-head hosted cycle.

Any failed gate in the additional cycle ends this authorization. Preserve
that failure and require a genuinely supported correction or an explicit
new decision; do not retry automatically, generalize this exception or
create another documentation-only opportunity.

### Delivery remains gated

Independent final integrated acceptance still binds all source identities,
the combined change and authenticated evidence. Only then may the PR squash
normally with the required trailers. Qualify the resulting actual-main
commit before expected-OID branch retirement; retain all existing stashes.
This exception does not authorize a retry or waiver of a future main failure.

A successful cycle establishes only that particular head's qualification.
It does not explain the historical host stall, prove future startup latency,
repair a Python implementation, or complete general generation, factoring,
IN query RHS, node43 or the project.

## Review and traceability

Review this decision for its exact failed-run binding, unchanged-input
precondition, single-cycle scope and retained gates before changing its
status to Accepted. Documentation-only review replaces no behavior test;
the accepted production sources keep their original TDD and complete-fast
evidence. No source-neutral action was authorized by the earlier rejected
same-head rerun proposal.

Retain the original raw CI archive, import/lifecycle records, authenticated
prefix control, failure analysis, rejected proposal and new design review
in the session evidence ledger. Publish the accepted exception and the
remaining qualification boundary in the PR before relying on the new
cycle. The final handoff must distinguish the failed original attempt,
the actual new execution and any unchanged local evidence reused.
