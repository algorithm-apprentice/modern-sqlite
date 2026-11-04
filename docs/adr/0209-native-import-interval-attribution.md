# ADR-0209: Native Import Interval Attribution

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source217 `594ad0f`, ledger217, draft PR #293
- Related contracts: ADR-0102/0106, ADR-0186 and ADR-0194/0195/0197/0208

## Measured observation gap

The ADR-0208 cycle is exhausted. Native Windows CI41774460218 attempt1,
job126391452981, times out the original Python owner at10.01s after2272
passing correctness cases. Its import markers reach harness readiness at
7.0139534s, including a4.6295854s `_hashlib` execution observer interval.
The original CI41769494913 failure also remains failed. Neither run is
retried, waived or represented as a nonexecuting infrastructure failure.

The existing observer records a begin marker before synchronously writing
and flushing it to stderr. CPython invokes that callback before the native
function. Subtracting the begin marker from its return marker therefore
includes the begin callback's output cost, not just the operation.

Parent-only controls on the unchanged source use the existing official
CPython3.12.10, preserve the original callback and measure its surrounding
boundaries. Injecting250ms into only the begin marker's writer produces a
252.174ms apparent `_hashlib` execution interval, while the callback-excluded
interval is1.5994ms. A separately slowed pipe reader likewise expands an
SSL execution interval to23.076ms while its callback-excluded interval is
about0.600ms. These are controlled attribution observations, not proof that
the hosted timeout was caused by logging.

The actual hosted logs record CMake4.4.5, while the canonical local native
CTest is4.3.1-msvc1. A separately downloaded, checksum- and publisher-verified
portable4.4.5 is used only for diagnosis, without changing PATH, presets or
caches. Both supervisors complete the unchanged38-method owner locally
under its original10s limit; version alignment alone does not reproduce the
hosted import stall. The hosted CTest executable itself was not recorded.

There is a second observation gap: passing CTest bodies are suppressed from
the main CI log. The current always-run publication step copies write-owner
progress, but not the Python import/progress records. A successful hosted
control therefore lacks the detailed phase evidence needed for comparison.

## Decision

### Separate callback reporting from the observed operation

Extend the existing `_ExtensionImportProgress` and
`_write_owner_progress`; do not add another profiler, importer, logger,
worker, watchdog, wrapper entry point or provider.

Keep the current begin/return/exception markers and their module-relative
wall/owner-CPU values. For each selected native call, measure the existing
begin reporter from immediately before its invocation until it returns.
Then retain the wall/CPU start for the operation. Capture the corresponding
end at the return/exception callback before invoking its reporter.

Publish these additional fields only with a matched return/exception:

- `begin_report_wall_ns` and `begin_report_owner_cpu_ns`;
- `operation_wall_ns` and `operation_owner_cpu_ns`.

The operation interval excludes the synchronous begin reporter and the end
reporter. Small profile dispatch/identity bookkeeping remains; do not call
it exclusively DLL-loading or exclusively OpenSSL execution time. Owner
CPU keeps the existing process-CPU meaning and clock-resolution limitation.
The original module-relative fields are unchanged and remain useful for
whole-import chronology.

Use one scoped pending-call stack in the existing observer, preserving
selected-call nesting and matching extension/operation identities. A missing
or mismatched pair is an explicit diagnostic error, not a fabricated zero
duration. Retire pending state when the scoped observer exits, including
failure. Reporting and clock failures propagate. An operation that raises
receives an exception record, never a successful return record.

Keep pre-existing-profile detection, the non-Windows no-install path, and
normal/exceptional callback retirement. Do not chain or replace another
owner's profile handler in the shipped implementation. The local experimental
callback wrapper is not production code and is not shipped.

### Publish existing records after the unchanged test owner

Extend the existing always-run native progress-publication step. Retain its
current name, write-model/write-session records, original temporary-root
observations and success-versus-earlier-failure checks.

Also copy the already captured `read-compatibility-progress` records and
the existing importtime entries for `_hashlib`, `hashlib`, `_ssl` and
`unittest.mock` from `LastTest.log`. A successful complete native selection
must have Python-owner progress; absence is an explicit failure. An earlier
failed selection may instead warn that it never reached that owner.
Do not invoke the owner again to obtain logs. Publication occurs after its
execution and cannot move import work outside the deadline.

Record the selected CTest executable path and its actual version alongside
the existing native toolchain versions. Do not select, install or pin a
different toolchain in the workflow.

### Preserve all original behavior and gates

This source changes diagnostic attribution and publication only. Preserve
all existing tests and real CLI boundaries, import order, interpreter
selection, `-X importtime`, `-m unittest -v`, directories, runner families,
processor reservations and10/8/1 limits. No prewarming, crypto replacement,
security exception, skipped workload or repeated CI attempt is introduced.

It does not fix or establish the underlying hosted startup-latency cause.
It supplies a demonstrated missing discriminator before choosing a supported
fix. ADR-0208 remains exhausted; this is a normally reviewed, test-first
diagnostic behavior change, not a source-neutral retry or policy-only reset.

## Test-first, review and delivery

Begin with a deterministic clock/output-delay control that observes the
missing callback-excluded fields on the unchanged observer. Cover successful
and exceptional native completion, nesting, pairing errors and retirement.
Keep existing direct/forwarded argument, busy-profile, non-Windows and
no-suite/no-watchdog import controls. Correct synthetic event sequences to
pair each return or exception with its own call; do not weaken their checks.

Exercise the actual PowerShell publication body against owned fixture logs:
preserve write progress, publish Python/import records, and reject a
successful selection that lacks Python progress. Existing earlier-failure
and complete-owner guards remain. Do not use substring presence alone as
evidence that the new records reach CI output.

Run the focused red/green and both complete Python owners at their unchanged
deadlines on the applicable platforms. Reconcile unchanged C++ sources,
images, configurations and complete fast evidence; do not claim new C++
execution for this diagnostic-only change. Run the actual selected tooling,
graph and discipline plan and retain every failed observation.

Independent source acceptance precedes a normal source218 commit and ledger.
Keep PR #293 draft until the required new-final-head hosted bodies and
integrated review pass. Qualified actual main and expected-OID retirement
remain mandatory. General generation, IN and node43 remain incomplete.

## Reference

- [CPython 3.12 profile events](https://docs.python.org/3.12/library/sys.html#sys.setprofile)
