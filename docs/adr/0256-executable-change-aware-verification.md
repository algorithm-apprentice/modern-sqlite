# ADR-0256: Executable Change-Aware Verification

- Status: Accepted
- Date: 2026-11-12
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #329 main `76bfd939`, ledger382
- Related contracts: ADR-0005, ADR-0102, ADR-0106, ADR-0213

## Problem and existing owners

The change-aware planner already selects documentation, tooling, targeted
and full verification. `run-shared` reconciles configuration-independent
tests, `run_fast_tests.py` owns bounded fast-local execution, and
`build-native` owns the native target set. Keep those mechanisms.

The missing operation is execution of the selected plan with one persistent
result. Recent delivery used per-PR scripts to translate build directories,
execute commands, preserve logs and reconcile results. This duplicated
orchestration and allowed a copied cleanup script to name the wrong branch.
Reference cleanup is not part of this proposal.

The changed-TU selector currently reports selected CMake object targets, then
returns the incremental build's exit status. Selection is not execution:
PR #329's planned analysis step compiled no object, and an independently
qualified supplemental invocation was required. A no-op is neither failure
of the compiler nor fresh analysis evidence.

The recent local plan already batched 2,200 ordinary cases into 18 processes
and executed 35 shared checks for 62 configuration instances. This decision
does not replace either optimization or delete their coverage.

## Decision

### Execute the canonical plan, not an arbitrary command document

Add `run` to `tools/verify.py`. It accepts the planner's existing change,
profile and GNU-directory options and a required report path. It generates
the plan itself through `make_plan`; it does not accept shell text or
user-supplied command arrays as a plan.

Execute the ordered commands synchronously, with argument arrays, in the
repository root. Preserve command arguments, test selectors, workload
limits and outstanding requirements. Do not parallelize CTest presets or
timings, introduce a scheduler, retry a failed step, or infer hosted/review
completion from local success.

Allow explicit mappings from known preset names to build directories.
Apply each mapping consistently to configure, build, CTest, generated
executables and report paths; never rewrite arbitrary substrings or infer
compiler identity from a directory name. Existing `--gnu-*-directory`
options retain their meaning. Explicit formatter/analyzer executable
options configure the existing quality preset; they do not install tools
or substitute a different checker.

Preflight all selected default, mapped and GNU directories before execution,
using resolved filesystem identities. Distinct configure presets must not
share a directory, including through symlinks. Reject an existing cache
owned by another checkout or incompatible build type, sanitizer,
instrumentation, fuzz, benchmark or analysis profile; do not silently reset
inherited options. A preconfigured GNU directory may alias the matching
development/Release owner only after its GNU compiler identity and complete
owned profile are verified. Record that alias as one physical configuration,
not independent compiler coverage.

Reuse `evidence_lock`, `write_json_report`, repository/environment identity
and command-file identity helpers. Reports and logs must live outside
source inputs, either outside the checkout or under an ignored directory.
Reject a tracked or unignored in-checkout report destination before running
commands. Give each invocation its own artifact directory.

Persist pending/running/passed/failed/not-run step states, exact commands,
durations, log paths and digests, and input identities. Write running state
before invoking a child and retain partial results on nonzero exit, launch
failure or interruption. Stop at the first failure. Source or environment
drift prevents a passed report. Local command completion is explicitly
different from complete project qualification.

The new executor and changed-TU report mode initially support POSIX hosts
(Linux, macOS and WSL), matching the existing fast-local/ASan/fuzz plan.
Native Windows retains its existing CMake/CTest verification, legacy
changed-TU invocation and planning/shared-check/native-target commands.
Unsupported execution fails explicitly before starting commands or creating
execution artifacts; it never falls back to weaker process supervision.

Catchable interruption or supervision failure must terminate only the
invocation's owned descendants and reap its child before final log hashes,
report finalization and lock release. Reuse the fast runner's POSIX
process-group cleanup as the canonical shared helper. Forward interruption
to nested supervisors so they retire their own groups before reaping the
outer owner. Do not kill processes by name or weaken CTest's own deadlines.
A real descendant-writer test must prove that cancellation ends writes
before evidence is finalized.

A native Windows trial exposed interpreter descendants outside the owned
Job Object, even with suspended launcher admission. Its failed evidence is
retained, not reclassified. A general native process-tree solution or Python
launcher substitution is outside this bounded efficiency batch.

There is no general cross-invocation test cache or automatic resume in this
slice. Existing matching evidence can still be reconciled under ADR-0102;
the runner must not invent reuse from a green exit status or old report.

The shared-cleanup refactor also has a Python performance-harness test
consumer. Its fault-injection seam must follow the canonical helper owner;
retain the original descendant, terminal-state and inspection assertions.
The intermediate draft CI exposed the stale test seam on Linux and macOS,
not an engine timeout. Preserve that failure and its focused reproduction.

A change confined to a recognized Python performance-harness test is tooling,
not a production performance mechanism. Match its existing CTest owner against
`configuration_independent_tests`; configure and execute that exact owner with
its unchanged properties. Do not introduce a second owner registry. Unknown
performance tests, performance drivers, C++ changes and mixed runtime changes
retain the existing conservative selections. This corrects unnecessary engine
and native-timing work without replacing the owning integration test with
generic tool tests.

### Make changed-TU analysis evidence explicit

Extend `tools/run_changed_clang_tidy.py` with an opt-in report mode used by
the local plan. Preserve the existing selection, whole-TU sharding and
legacy invocation when report mode is absent. Empty assignments explicitly
report no analysis, rather than a fresh successful checker execution.

Report mode requires a configured Ninja quality build belonging to the
current checkout, with clang-tidy enabled. Keep the existing CMake
compiler-plus-analyzer command. Use Ninja's generated commands and dependency
records, rather than introducing a separate analyzer invocation.

A matching previous successful report may be reused only for the exact
assigned targets, command/configuration, tool executable bytes, environment,
non-documentation repository inputs, object bytes and recorded compiler
dependencies. Re-read dependency content, including dependencies outside the
repository. Missing, stale, malformed or incomplete dependency records do
not authorize reuse. Positively verify a full dependency-emission mode as
well as the records: Ninja's MSVC dependency output alone is not proof of
the system-header closure. Unsupported modes run fresh analysis with
explicitly non-reusable evidence; fresh MSVC analysis retains its original
configured command. Preserve the original execution identity and record
reuse as reuse, never as new analysis.

Require the complete recorded dependency paths and bytes to agree before and
after a fresh execution before marking that execution reusable. A cold or
changed dependency closure can produce fresh analysis evidence, but not
reusable evidence from a post-execution snapshot alone. File timestamps do
not prove unchanged input bytes.

Without a matching reusable report, explicitly rebuild only the selected
objects: validate that their resolved paths are contained in the configured
build directory before removing those generated outputs. Do not clean the
build tree or remove unrelated targets. Canonical build-relative Ninja
target keys are separate from resolved filesystem paths used for hashing,
containment and removal. Normalize compilation-database outputs before
queries or builds; never pass their absolute filesystem names as Ninja keys.
Serialize report-mode operations on the same build directory as well as
the report path. Capture verbose CMake output and
require each selected generated compiler-plus-clang-tidy command to execute
successfully. Record fresh execution and its inputs; a successful command
without complete selected analysis remains incomplete evidence.

The record is not a cache for diagnostics from another compiler, runner,
configuration, source or environment. Changed dependency bytes invalidate
reuse even if timestamps were preserved. Do not weaken the fingerprint to
increase cache hits. Raw diagnostics and all configured checks are retained.

The local targeted plan supplies all explicit changed sources to one selector
invocation. Repeatable `--smoke-source` arguments retain uncommitted sources
without running the same committed changed-TU union once per source. Reconcile
the prior report under the same build owner and preserve one fresh/reused
decision for that assigned union.

### Keep documentation navigable

Use the existing development handoff as the current portable entry point.
Keep historical handoff evidence separately labelled and linked. Correct
README delivery guidance, put the audit's current open boundaries ahead of
historical closure statements, and add an ADR reading map for current work.
Do not delete or renumber historical ADRs or claim new SQL behavior.

Workflow rules remain in `AGENTS.md` and the accepted delivery ADRs;
verification usage remains in `docs/verification.md`; graph state remains in
the project JSON files. Link these owners rather than copy another rule set.

## Verification and delivery

Develop executable behavior test-first in the existing verification and
changed-clang-tidy test files. Cover partial failure, input drift, report
ownership, directory mapping, outstanding requirements, fresh/no-op/reused
analysis, header/tool/configuration changes, escaped object paths and empty
shards. Include symlink/configuration alias and incompatible-cache controls,
real descendant cancellation and small Ninja-fixture controls, not only
mocked command assertions.

Run focused cases and the complete owning Python/tool tier. Documentation
requires reference/graph review; unchanged engine runtime is not new local
execution credit. Apply the actual change-aware closing plan and required
hosted gates. Preserve every existing test deadline, assertion, workload,
failure-injection owner and compiler domain.

Deliver small related commits under ADR-0106: current documentation/design,
plan execution, and truthful analysis evidence. Independently review design
before behavior changes and implementation before committing. Qualify the
final exact batch head. Report measured execution/reuse and fewer manual
orchestration steps, not an assumed whole-project speedup.

This decision does not change GitHub protection, workflow scheduling,
partial-rerun authority, source retirement or SQL compatibility. Full private
source preparation and `NC_NoSelect` remain the next SQL work after this
bounded tooling batch.
