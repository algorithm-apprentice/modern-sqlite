# ADR-0223: Observable Quality Bootstrap Phases

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Scope: PR #301, quality-bootstrap diagnostics after accepted source258
- Related contracts: ADR-0102/0106, ADR-0211/0212/0213/0214/0217 and ADR-0222

## Observed failure and limits of the evidence

Source258 is independently accepted at
`86db9437813fa6c71eaf127b99f2431baaa3fc45`, tree
`07b516ab76652f844f32c2fae7d48213302ab8c9`.
Its owning metadata red/green, complete native fast units, selected local
plan, genuine GNU Release coverage, matched controls and allocation evidence
are recorded. Source acceptance is not completed delivery.

Original CI42274043192 attempt1 concludes cancelled. Configured
`macos-26-intel` worker `quality-shard (0)`, job128085649722, has the explicit
annotation that its original twenty-minute job budget was exceeded.
Its last reported step is `Install quality tools`, still marked in progress.
Configure, changed-source analysis and subsequent work are not recorded as
executed and receive no qualification credit.
The fail-closed aggregate quality job correctly fails.

The retained completed-run ZIP has134 entries but no entry for that worker.
One completed-job log request returns HTTP404; the response and request
identity are retained. No terminal-escape override or alternate access path
was used. These observations identify the missing qualification and its
reported phase, not the stalled installation command or its cause.

That step contains the architecture guard, verified CMake bootstrap,
Homebrew tool installation and the formatter virtual environment. Without
its body, no particular package, download, host, network, scheduler, security
product or engine defect can be blamed. The failed worker's actual checkout,
image details, compiler and selected tools are not reconstructed from another
host's successful logs.

Successful original peer/control bodies are retained separately:

- Four executed ordinary runtime populations each contain2326 passing cases.
  Windows has19 owning contracts,2316 correctness cases and two verification/
  workflow-tooling cases. Intentional PR body skips remain nonexecutions.
- The other nonempty quality worker completes its assigned source, and the
  empty worker completes shared checks without being counted as analysis.
- Original supplemental42274040752 attempt1 completes its four actual jobs:
  full production analysis with44 distinct objects and both libraries,
  platform populations2299/2320, and four20,000-run/seed102 fuzz campaigns.

Those observations are successful controls, not replacement credit for
`quality-shard (0)` or evidence that the original CI passed. Different hosted
instances are not assumed to have identical runtime conditions.

## Confirmed diagnostic gap and existing owners

The standard quality workers and full-production quality job contain the
same installation command sequence in one named step. GitHub's retained
step metadata cannot distinguish architecture validation, CMake bootstrap,
Homebrew, formatter setup or final PATH publication when that step's log
body is unavailable.

The canonical `tools/install_ci_cmake.py` already validates its owned paths,
download size/hash and complete executable pair before appending the verified
binary directory to `GITHUB_PATH`. Its own step still needs the existing
explicit PATH export before printing CMake/CTest versions; later steps receive
the published directory. No new installer or provider is needed.

The owning policy tests already check architecture-before-installation,
verified tool selection, complete PATH publication, fail-closed quality gates,
budgets and workflow triggers. Reuse those tests and the existing installer.

## Decision

### Expose five ordered job-step boundaries

Replace the combined installation step in `.github/workflows/ci.yml`'s
`quality_shards` job and `.github/workflows/full-quality.yml`'s `full-quality`
job with these same five named phases:

1. `Verify quality runner architecture`: retain
   `uname -m | grep -Fx x86_64`.
2. `Install verified quality CMake`: retain the shared installer invocation,
   current-step PATH export, and both executable-version checks.
3. `Install quality compiler tools`: retain `brew install llvm ninja`.
4. `Install quality formatter`: retain the existing virtual-environment
   creation and `clang-format==23.1.2` installation.
5. `Publish quality tool paths`: append the existing LLVM directory and
   formatter directory to `GITHUB_PATH`, in their current order.

Every phase uses the existing explicit Bash fail-fast/pipefail shell.
Keep the commands, arguments, working directory and relative ordering.
Do not publish LLVM/formatter paths before both installations succeed.
Do not activate a virtual environment or add another interpreter-selection
rule. The shared CMake helper remains the sole installer and validated
publication owner; its code, download profile and failure cleanup are unchanged.

Use actual job steps rather than another composite wrapper that would hide
the same boundaries behind one outer step. Add no package manager, helper
framework, runner, cache, polling process or environment-dependent fallback.
The existing runtime/benchmark macOS setup steps are not changed by this
bounded quality-owner correction.

### Preserve fail-closed behavior and budgets

Keep all runner labels, workflow permissions, concurrency rules, job budgets,
quality targets, shard assignments, tests, assertions, allocation ceilings,
campaigns and deadlines unchanged. A failed phase prevents subsequent phases
and validation as before. Do not add retries, `continue-on-error`, warmup,
security exclusions, a different provider or longer limits.

The extra shell boundaries add diagnostic identity, not a speed improvement.
They do not explain or repair the historical installation timeout. Future
metadata can identify the last reported phase more precisely when available;
this is not a promise that a lost runner will always upload complete logs.

Recorded tool provenance now comes from its actual contributing phase bodies.
The architecture guard, verified CMake record, Homebrew compiler selection,
formatter setup and PATH publication must all be checked in order.
Do not accept a missing phase, silently fall back to the old combined-step
decoder, or infer the failed original host from a later successful instance.
An empty analysis shard still executes setup/shared responsibilities without
being counted as a compiler/analyzer execution.

## Test-first and qualification

Add one focused owning workflow-policy regression requiring the five named,
ordered phases and their original commands before changing either workflow.
Record its genuine failure. Update the existing verified-CMake policy check
to recognize the separate architecture and CMake owners while retaining the
unchanged runtime/benchmark setup checks.

Verify fail-fast Bash shells, architecture before installation, canonical
CMake selection/publication, delayed LLVM/formatter publication and unchanged
job/test/workload limits. Retain all existing installer fault, invalid-path,
hash/version, cleanup and quality-gate controls. Do not replace executable
negative controls with comments or broaden permissions to run them.

Run the owning policy suite and the actual change-aware closing plan.
Reconcile unchanged compiled production, configuration, image, selection and
execution inputs under ADR-0102 rather than relabelling or unnecessarily
rebuilding source258's accepted engine evidence. Changed workflow/policy
inputs require their own fresh qualification. No new native macOS execution
is claimed from local policy tests.

Independent design acceptance precedes implementation. Independent source
review and a normal coherent source259 commit/ledger entry precede one fresh
exact-head qualification cycle. Keep PR #301 draft; preserve original
CI42274043192 and supplemental42274040752 attempt1 and never rerun them.
This is a supported observability correction, not a documentation-only retry
exception or an extension of ADR-0208.

The new final head must pass all twelve standard contexts and four actual
supplemental bodies with current checkout/profile/population authentication.
If another gate fails, retain it and diagnose its actual evidence; this
decision authorizes no automatic retry loop or policy-only reset.

Independent integrated review must bind sources258/259 and the complete final
union before normal squash. Qualify resulting main, bind current and original
raw archives, refresh execution/job/step guards and enforce expected-OID
retirement while preserving every pre-existing stash.

The original timeout cause remains unresolved unless separately established.
This diagnostic correction admits no SQL/generation behavior and does not
complete IN, node43 or the project.
