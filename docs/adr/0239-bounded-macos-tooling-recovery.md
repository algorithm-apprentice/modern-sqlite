# ADR-0239: Bounded macOS Tooling Recovery

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Blocked ancestor: PR #313, main `bc5c8ff7`, tree `81847ddc`, ledger315
- Qualified ancestor: PR #312, main `f7e15cd7`, ledger312
- Related contracts: ADR-0102/0106/0208/0212/0227/0236/0238

## Observed failures and completed owner inventory

ADR-0238's source313 and integrated tree pass their actual selected local
plan, original premerge standard/supplemental bodies and independent review.
The normal squash preserves that tree. Its actual-main qualification is
nevertheless incomplete.

Original CI42579117784 attempt1 has two distinct failures:

- Mac Release job129033423559 times out `tools.ci_workflow_policy` at10.19s
  under the original10s limit. It records2358 passing cases and leaves18
  cases unexecuted under the unchanged stop-on-failure policy.
- Mac native-contract job129033423629 receives HTTP504 while downloading the
  pinned CMake archive. Executable verification, PATH publication and the
  native workload do not run.

A separately reviewed one-shot diagnostic is exhausted. Attempt2 runs only
the two named failed roles; eight represented records retain their original
timestamps, steps and runners. Fresh native job129048996468 passes the
verified4.4.5 bootstrap and all55 native cases. Fresh Release job129048996830
instead receives another HTTP504 during bootstrap, before Configure, Build
or Test. Its Runner/Worker records are retained. The original policy timeout
is neither reproduced nor explained.

The original policy owner on that Mac host has no import/method progress.
Same-tree premerge Release and main Debug controls pass, but different hosts
are not matched causal experiments. Local original10s controls also pass on
Windows and Linux, with different Python/CMake profiles. Their declaration,
version and image preflight may warm caches; they are not cold-start or
matched-macOS evidence.

HTTP504 identifies an unsuccessful gateway/upstream response, not the
responsible network component. There is no evidence for an engine deadlock,
an interpreter defect or a security-product cause. Automatic main Full quality
success does not replace either failed owner.

The existing canonical mechanisms are sufficient:

| Responsibility | Existing owner |
| --- | --- |
| Fixed HTTPS URL/version/size/SHA256 | `tools/install_ci_cmake.py` |
| Unique job-temporary prefix and failure cleanup | `install_profile`, `ExitStack` |
| Complete archive, executable-pair and PATH publication checks | `install_profile` |
| Shared bootstrap in all macOS installation bodies | ADR-0212, both existing workflows |
| Real archive/tool/publication failure fixtures | `CiWorkflowPolicyTest` |
| Combined10s policy-owner budget | `tests/CMakeLists.txt`, `tools.ci_workflow_policy` |
| Native import/method progress | ADR-0236's Python/unittest arguments |
| Native-only process-observer ownership | ADR-0227/0236 |

This decision introduces no second installer, test runner, timing wrapper,
interpreter/provider choice or general retry framework.

## Decision

### 1. Recover once from the observed pinned-download opening failure

Extend only the shared CMake response-opening boundary. Its first HTTPS GET
retains the fixed URL and original60s timeout. If that opening operation raises
`urllib.error.HTTPError` with code504, close the error response and permit at
most one further opening of exactly the same URL.

Start a monotonic opening budget immediately before the first attempt.
Subtract the actual elapsed time from60s before considering the second attempt.
If no positive budget remains, propagate the failure without another request.
The second timeout must be no greater than the remaining budget; it must not
reset to60s. Do not retry a failed second attempt.

This is bounded recovery inside one installer invocation, not permission to
rerun a workflow or qualification owner. It does not claim a new hard wall-clock
guarantee for DNS, redirects or all streaming reads. Preserve the existing
`urlopen`/stream behavior; do not extend any timeout or job budget.

Report the first504 and the single recovery decision to stderr before the
second request. Stdout remains reserved for the successfully verified binary
directory. A final failure continues through the existing explicit error/
nonzero-exit path, with no success-shaped PATH fallback.

Other HTTP codes, certificate/transport exceptions, stream-read failures and
partial bodies do not trigger recovery. Nor do incorrect size/hash, extraction,
missing executable, version mismatch or PATH-publication failures. Do not add
another URL, mirror, package manager, archive format, cached result, tool
version, provider or installation route.

Open the archive file only after the response opening succeeds. Keep one
existing owned prefix, complete bounded size/SHA256 verification, both tool
identity checks and one final PATH publication. Close every retained HTTP error
response before retrying or propagating it. Preserve all unrelated temporary
contents and the original cleanup owner.

### 2. Make the same policy progress visible on every supported host

Use the existing configured Python `-X importtime -m unittest -v` command
for the single canonical `tools.ci_workflow_policy` registration on every
supported platform. Windows already has those arguments; its argument list
must remain unchanged. Other platforms gain only the same two diagnostic
options.

Keep the same interpreter, module selector and repository working directory.
Retain the combined `TIMEOUT 10`, labels, processor reservation and ordinary
failure propagation. Add no readiness reset, wrapper, process, sampler,
warmup, preimport, environment substitution or automatic test rerun.
All imports, discovery, methods, fixtures and child commands remain inside
the same CTest clock.

Preserve all original native32/non-Windows31 policy methods and their
assertions. Any added downloader behavior controls remain owning tests with
explicit population accounting; no existing method is removed or skipped.
The adjacent verification owner's30s budget and four native-only process
owners'10s/two-slot contracts remain unchanged.

This narrowly supersedes ADR-0236's Windows-only diagnostic-argument scope,
not its timing, workload, native-only observer or failure-retention rules.
The original non-Windows argument list intentionally changes; do not describe
it as unchanged evidence.

Completed import timing and verbose method lines provide phase information,
not a native stack or a causal diagnosis. Logging has overhead. No latency
repair, scheduling equivalence or explanation of the historical timeout is
claimed.

### 3. Preserve the delivery boundary

The original failed attempt and exhausted diagnostic remain failed.
ADR-0208 and prior diagnostic decisions provide no further allowance.
This ADR authorizes no source-neutral workflow request.

Admit one named two-slice corrective tooling batch from blocked main
`bc5c8ff7` under ADR-0106:

1. Add the bounded pinned-download opening recovery and owning controls.
2. Extend the existing policy progress arguments and exact registration
   controls to supported non-Windows hosts.

These are related verification-owner corrections, not new SQL development.
Keep each slice buildable, independently reviewed and normally committed
with complete focused/fast evidence and an append-only ledger record before
the next slice. Keep the corrective PR draft until its final head is accepted.
Retain source313's reference and all three existing stashes.

Every production C++ input and owning engine test remains unchanged from the
reviewed ancestor. Reconcile only genuinely identical source/input, compiler,
configuration, binary, selection and environment evidence. Changed shared/
tooling instances must execute; do not reuse their earlier success.
Select the actual final closing union covering the retained engine prefix
and corrective batch from qualified ancestor `f7e15cd7`. A plan is not execution.

Require actual final-head standard and supplemental bodies, independent
integrated acceptance and normal squash, then actual descendant-main
qualification. Only separately reviewed ancestry/input reconciliation,
complete descendant execution, annotations and expected branch OIDs may
close PR #313's delivery block and retire both retained source references.
Neither bootstrap recovery nor a diagnostic log alone qualifies the engine.

General deferred/reusable generation, signed literals, preparation-error
arbitration, mutation-generation work, IN and later DAG nodes remain blocked.

## Focused proof

Before changing the installer, extend the existing real verified-archive
fixture with one first504/then-valid-response success case. Record its current
failure. Keep normal first-attempt publication and every existing failure
control, and cover exhausted budget, another failed opening, noneligible
errors, response closure and invalid data after recovery.

Verify at most two same-URL calls, the original first timeout, the decreasing
remaining timeout, explicit stderr progress, no premature PATH publication
and complete owned-prefix cleanup. Use deterministic network/clock fixtures,
not a live-network unit test or a relaxed timeout.

Before changing CMake registration, require the diagnostic arguments in its
real Windows-enabled and Windows-disabled fixture and record the missing
non-Windows arguments as red. Verify the complete original budget, labels,
directory, method selection and neighboring owner contracts. Execute the
actual policy owner and inspect complete method accounting and progress
without moving work outside its clock.

Run the applicable complete tool selections, original focused/fast tiers,
exact selected closing checks and required hosted gates. Keep failed,
diagnostic, local-control and successful qualification evidence distinct.

## References

- [HTTP504 semantics](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.6.5)
- [Idempotent requests and bounded automatic retry](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.2.2)
- [Python HTTP error response ownership](https://docs.python.org/3.14/library/urllib.error.html)
