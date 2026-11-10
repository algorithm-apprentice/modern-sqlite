# ADR-0243: Bounded Policy-Fixture Scheduling

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source330 `bc63631c`, ledger330, draft PR #317
- Qualified delivery base: PR #316 main `2ea97ddc`, ledger329
- Related contracts: ADR-0102/0106/0203/0210/0227/0236/0239/0242

## Observed failure and completed fixture inventory

Source330 passes independent source review, all244 owning and2340 rebuilt
native unit cases, and its complete targeted20 local closing plan. Its
original supplemental workflow reports success. Original standard CI
42702447674 attempt1 nevertheless fails Mac Release job129450408299:
`tools.ci_workflow_policy` reaches its unchanged10s limit at10.12s.
CTest executes2367 of2385 cases, with one failure and18 unexecuted cases.
The failed run and its complete archive remain retained; no retry is requested.

The policy output contains27 completed method lines out of31 and1.069616s
of completed import self-time. The scheduler starts `tools.verification`
before the policy timeout; that other owner completes in15.90s. These facts
do not establish contention, a deadlock or the phase of a missing method.
The original Windows policy owner instead passes in3.92s, and all six Windows
tooling owners pass. Do not describe this event as a Windows engine failure.

Two separately recorded local diagnostics use fresh processes under the same
10s combined limit and an observational unittest result subclass. They retain
all original methods, assertions and real fixture commands. Linux completes31
methods in1.77183s of CTest case time; Windows completes32 in9.97753s.
Windows outer driver time is10.04889s, not the test-process timeout clock.
These are local observations, not canonical qualification, cold-cache claims,
matching macOS experiments or explanations of the historical failures.

One avoidable serialized cost is established directly in the current owner.
Three methods execute twelve independent real CMake configuration/discovery
fixtures. They consume0.69408s of method wall time in the Linux diagnostic and
3.87821s in the Windows diagnostic:

| Existing owning method | Independent configurations |
| --- | --- |
| `test_native_startup_contracts_have_isolated_windows_owners` | WIN32 enabled/disabled:2 |
| `test_native_import_profile_changes_only_the_declared_python_owner` | WIN32 enabled/disabled:2 |
| `test_sanitizer_native_differentials_retain_uncontended_deadlines` | APPLE, Debug/Release and ASan combinations:8 |

Each fixture owns its temporary root, CMake file, build directory, cache,
tool processes and captured output. It reads immutable registration text,
repository files and interpreter paths. Configure precedes discovery within
that fixture. The three methods do not use mutable module mocks while running
these tools. Other policy methods do use mocks and remain strictly sequential.

The owner currently reserves one CTest processor. Existing bounded fixture
scheduling and two-slot owners provide prior art in ADR-0203 and
`tests/CMakeLists.txt`. CTest's PROCESSORS property accounts scheduling slots;
it does not itself create threads or promise operating-system CPU isolation.

## Decision

### Bound the existing real fixture batches to two workers

Use the standard `ThreadPoolExecutor` with exactly two workers for each of
these three existing configuration matrices. Submit the real configurations
before collecting their results; retain the original variant order for
unittest subtests and metadata assertions.

Extract one small fixture helper in the existing policy test module and use
it for all three matrices. The helper owns one temporary directory, writes
the same complete CMake project, configures it with the unchanged Ninja
command, and discovers its actual CTest metadata. Preserve both tool
return-code checks and their captured failure diagnostics. A failed
configuration must not become an empty successful declaration or execute
an unrelated fallback.

A worker retires its complete private fixture before returning detached
metadata. Tool failures propagate to the owning unittest subtest. Keep
assertions and subtest reporting on the main unittest thread, and wait for
all submitted work on every exit, including a failed worker or assertion.
Do not return a lazy generator that outlives its executor or fixture owner.

Keep all twelve configurations, full registration snippets, exact command
arguments, test-name sets, paths, labels, timeout and scheduling-property
assertions. Do not replace real configuration/discovery with text parsing,
cached metadata, fake successful commands or a single shared build directory.
The policy owner retains its original32 Windows/31 non-Windows methods;
no method is split, removed, skipped or run concurrently with another method.

Do not parallelize installer, storage, publication or other mock-based
controls. Add no configurable worker count, general test scheduler, new
production module, interpreter choice or runtime container.

### Reserve the corresponding canonical owner budget

Set `PROCESSORS 2` on the existing `tools.ci_workflow_policy` registration
on every supported host. Under the existing two-worker CI test selection,
its two child-tool slots are not also assigned to another CTest owner.
With larger externally selected worker counts, this is still a two-slot
reservation, not an assertion of global isolation.

Keep the configured interpreter, `-X importtime -m unittest -v` command,
working directory, labels and combined `TIMEOUT 10` unchanged. All imports,
discovery, fixtures, output and shutdown remain inside that same clock.
Add no warmup, preimport, readiness reset, timeout extension or provider
replacement. Preserve the adjacent verification owner's30s budget and all
native startup/read-compatibility reservations.

This narrowly supersedes ADR-0236/0239's unchanged policy-owner reservation
and the three fixtures' serial implementation. It does not claim that either
change explains the original Mac or Windows timeouts, guarantees wall time
on every host, or authorizes an unchanged-source workflow retry.

## Test-first proof and bounded delivery

After independent design acceptance, change the existing real registration
control to require the two-slot policy owner. Record its current one-versus-two
failure before changing CMake. Preserve every neighboring owner assertion.
Treat the fixture implementation as a green refactor over the measured
original configuration population, not an artificial behavior red.

Verify every original configuration and result assertion, bounded genuine
fixture overlap, ordered result collection and full cleanup on success and a
tool failure. Keep the tests meaningful: do not merely assert the executor's
constructor argument or re-test standard-library behavior.

Execute the complete native and non-Windows policy owners under the unchanged
clock and all existing tooling selections. Compare the same bounded phase
observations without calling them macOS qualification or a speed guarantee.
Any diagnostic that fails remains failed and is not automatically repeated.

Admit one coherent source331 verification correction after accepted
source330 in the same related PR #317 delivery under ADR-0106. No production
C++ or owning engine test changes. Reconcile genuinely identical engine
inputs, binaries, test declarations and configuration evidence; changed
tooling instances must execute.

Run the actual change-aware final union from qualified main `2ea97ddc`;
retain any escalation caused by registration changes. A plan or a local
diagnostic does not replace the required fresh exact-head Mac Release body,
the previously unexecuted cases, other standard/supplemental owners,
independent integrated review or separate actual-main qualification.
Preserve the original source330 failure and all three existing stashes.

General native generation, the nine deferred callback differences, two
unrequested constructor gaps, IN and later DAG nodes remain incomplete.

## Reference

- [CTest PROCESSORS scheduling property](https://cmake.org/cmake/help/latest/prop_test/PROCESSORS.html)
