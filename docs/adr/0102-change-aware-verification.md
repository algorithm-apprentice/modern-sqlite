# ADR-0102: Change-Aware Verification

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

The user requested an executable improvement to development efficiency after
observing nearly half-hour local qualification for small slices.
ADR-0005 already requires a fast inner loop, focused red/green behavior, and
the complete fast unit tier. It does not require every local configuration,
long campaign, and native benchmark build after every edit.

A read-only audit of successful post-PR #210 qualification measured:

- 29.75 minutes in one serialized local closing chain;
- 22 configuration-independent Python command groups with 104 extra
  executions, representing about 31.88 seconds of repeated work;
- a default-ALL benchmark build that additionally linked 39 unit/model/
  allocation executables;
- identical ordinary Release/benchmark compiler arguments for the three
  touched production translation units, but a genuinely different diagnostic
  engine with instrumentation/probes; and
- 1,344 normal GNU Debug GoogleTest processes taking 226.95 seconds, compared
  with 7.51 seconds of reported test-body time.

A diagnostic execution of the same binary and 1,344 names in one process
passed in 7.71 seconds. It did not preserve process isolation or per-case
supervision, so it was not accepted as replacement qualification.

The existing mechanisms to reuse are CMake/CTest discovery and properties,
GoogleTest, the pinned native contracts and target list, standard-library
Python subprocess supervision, project/ADR metadata, and exact input
fingerprints. Add no second test framework, engine executor, plugin system,
unbounded cache, or guessed compatibility layer.

Begin after PR #211 at main commit
`fd693a8d45d192e92ed888670d3e76e7a980dbbf`, accepted tree
`67bee264a726fbf8d3254b4933e21409f2a3e054`. All nine exact-head checks
passed; the original unchanged sanitizer model passed in 2.52 seconds under
its ten-second limit. Its completed local evidence was reused after the
verification audit, not rerun.

## Decision

### 1. Keep distinct evidence domains explicit

Separate the development inner loop, risk-selected local closing checks, and
isolated qualification/hosted gates. A plan is not evidence of execution.
Successful fast-local execution is not an isolated sanitizer/compiler gate.

Every changed production behavior still requires its focused red, focused
green, and complete fast unit tier. Refactors start and remain green.
Documentation-only updates require review/reference/graph checks, not C++
rebuilds. Unchanged successful work is not repeated merely because review
finished or documentation was edited.

Keep every current required hosted job, test assertion, malformed/failure
case, workload, allocation ceiling, benchmark threshold, and deadline.
The original source-correlated model, model/fault/crash executables, and
special exit-status tests retain their isolated CTest execution.

### 2. Publish one conservative change-to-validation plan

Add one standard-library Python verification command and one small reviewed
policy document under `project/`. The policy owns the source-domain mapping,
configuration-independent test names, and benchmark target list.

The plan reads actual Git changes, including staged, unstaged, untracked,
deleted, and renamed paths. It emits structured commands and coverage
requirements rather than silently executing or claiming success.
Mixed changes take the union of their domains. Unknown paths and shared
foundation/cache/toolchain and durable mutation/recovery changes escalate
to full qualification.

Use these minimum selections:

| Change | Additional local evidence beyond the required production fast tier |
|---|---|
| Documentation only | Document/reference/ADR review and graph consistency |
| Test/tool/harness | Own unit/integration/negative controls and test discovery |
| Binder/planner/lowering/VM | Native SQL rows/types/errors, lifecycle/model coverage, affected sanitizer/GNU tests; SQL fuzz for grammar changes |
| Codec/page/cursor | Goldens, malformed/OOM/ownership/pin/I/O, affected sanitizer/GNU, native interoperability, database fuzz |
| Pager/journal/transaction/writer | Deterministic failure/crash ordering, recovery, bidirectional images/integrity, broader affected compiler coverage |
| Shared public headers/base/toolchain or unknown | Full relevant compiler/configuration matrix |
| Performance mechanism | Correctness/failure controls, fixed work and matched isolated timings |

Always retain the original ten-second model in applicable sanitizer/hosted
closing evidence. Additional instrumentation/long SQL and database campaigns
are selected by affected mechanisms or explicit full qualification, not
blindly attached to documentation and tooling changes.

Compiler, sanitizer, process mode, seed, and input differences are independent
coverage, not duplicates. Missing tools/configurations and empty or invalid
test selections are explicit errors or outstanding requirements, never green
fallbacks. A caller may request broader validation, never an automatic
lower-risk override of an unknown change.

### 3. Run configuration-independent checks once in a local plan

Append an additional CTest label to explicitly inventoried Python/tool/
pinned-oracle checks. Preserve their existing names, labels, commands,
working directories, assertions, timeouts, and registration conditions.
Their canonical names come from the shared policy, not another copied list.

One local plan contains one shared-check step; later compiler/runtime and
native selections exclude that label. Coverage is the exact union, not
silently skipped tests. Hosted CI continues to run its current full CTest
selections.

Before exclusion, the executable shared step reads every selected configured
CTest tree. The owner must contain every excluded name with identical
command, runtime properties, timeout, directory, interpreter/tool and
environment identity. Missing optional owner tests or differing identities
fail before execution, rather than losing coverage. Record and recheck source,
consumed-file and environment identity around the shared run.

This first delivery adds no automatic cross-invocation evidence cache.
Reuse of prior evidence still requires exact recorded source/input, binary,
tool, configuration, environment, selection, timeout, and seed identity.
Failed, skipped, incomplete, or differently configured results cannot satisfy
another domain.

### 4. Build only selected benchmark contracts

Move the existing CI benchmark target list into the canonical policy.
The verification command builds exactly those targets through CMake using
argument arrays and a supplied build directory/job count.
Local instructions and the existing CI build step consume that same list.
Do not change the CI test selector, workload inputs, timing isolation, or
diagnostic/timing separation.

Do not merge the diagnostic engine with the ordinary Release library.
Cross-directory ordinary-library artifact sharing is deferred until compiler,
flags, generated provenance, dependencies, and environment are proven equal.
Avoiding unrelated default-ALL targets is the bounded current optimization.

### 5. Add a bounded fast-local unit runner, not a new qualification gate

Provide an opt-in local fast runner based on CTest's discovered `unit`
selection. Only the explicitly known ordinary `modern_sqlite_unit_tests`
executable may be batched. All other unit executables retain CTest invocation,
per-case properties, isolation, resource locks, and expected-exit behavior.
Unknown or exceptional ordinary-test properties are left isolated.

Partition eligible ordinary tests into bounded batches of at most 128 names,
preserving actual discovery names and common executable/arguments/directory/
environment. No assertion, case, repetition, input, or selected timeout is
removed. Deliberately do not claim per-case process isolation for this layer.

Supervise each batch through the pinned GoogleTest start/end protocol:

- every started name must belong to the expected selection and must complete
  exactly once before another begins;
- enforce each discovered per-case timeout using a monotonic deadline;
- bound startup, between-case, shutdown, and captured output;
- terminate and reap only the owned process group on failure/timeout, leaving
  no test descendant running;
- require successful process exit, exact XML and protocol case accounting,
  and zero failed/error/skipped/disabled cases; and
- fail on missing, duplicated, unknown, malformed, or timed-out cases without
  silently retrying them in an isolated process to manufacture success.

CTest executes the remaining unit selection separately. Aggregate evidence
must equal the complete discovered unit set with no omissions or duplicates.
The report explicitly records fast-local process mode, binary/input identity,
executed names, limits, elapsed time, and non-equivalence to isolated gates.
Lock each report path for its entire invocation and bind artifacts/writes to
one run identifier. Persist normalized discovery/limits and hashed environment
identity, hash every invoked executable/input, and recheck identity after
execution. Malformed structured input finalizes failed evidence; partial
successful and failed case outcomes are retained.

Keep existing CTest presets and hosted test commands unchanged. Models,
failure-injection executables, and the original sanitizer deadline are not
folded into this batch. Qualification still supplies process isolation.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before implementing policy/tool behavior.
Develop observable CLI/planning and supervision behavior test-first through
the existing unittest style without new dependencies.

Cover documentation, mixed and unknown changes, deletions/renames/untracked
files, source-domain unions, full escalation, missing configuration, empty
selection, canonical target safety, unchanged shared-check coverage, and
explicit plan-versus-execution state.
Tool/test changes select their actual existing owners or conservatively
escalate. Manifest automation changes require tooling checks and explicit
review. Production plans retain focused red/green or green-refactor-baseline
requirements. Durability/full campaigns include existing writable SQL and
database fuzz targets as well as read targets.

Use bounded fake executables to test successful batches, stdout/XML accounting
errors, failed/skipped tests, process failure, timeout, missing events,
capture limits, cleanup, and preservation of standalone test properties.
An unchanged isolated-gate result cannot be inferred from these fast reports.

Reconcile test discovery in the existing configurations: retain every
inherited test command/property except the additive shared label, plus new
tool tests. Prove the CI native target set and all required job/test selectors
remain equivalent. Verify the ordinary and diagnostic compile flags remain
unchanged.

Run the realistic complete fast-local selection and compare exact names,
failures/skips and elapsed time with the recorded same-work baseline.
Report measured scope and limitations, not a production-engine speedup.
Run tool/integration, graph, discipline, format, and applicable isolated
closing checks; do not mechanically rebuild six unchanged engines to test a
Python planner. All nine exact-new-head hosted checks remain required.

Review implementation and exact tree with GPT-5.6 Sol before guarded squash
merge and verified named cleanup. This workflow prerequisite does not close
remaining audit findings, node 43, or later nodes.

## References

- ADR-0005: Test-Driven Development
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0101: Typed Read Allocation Boundaries
- `AGENTS.md`: development, testing, and completion contracts
