# ADR-0183: Single-Process Windows Oracle Fixtures

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: ADR-0182 sources `cf82ec2` and `66a0c51`, ledger 124-125
- Related contracts: ADR-0102, ADR-0106, ADR-0153, ADR-0178

## Completed inventory and retained failure

ADR-0182's two normal source commits are independently accepted and locally
qualified in draft #276. Source 2, tree `22d34e4`, passes 71 focused cases,
all 2,194 native fast units and 2,231 unique native cases, plus the 2,175-case
affected instrumented-production selection. The original owning diagnostic
now matches all ten native default-ABORT controls. Node 43 is not complete.

Informative prefix CI `41125944050` at `cf82ec2` instead fails the existing
Python-only `differential.read_compatibility_tool` owner at 10.01 seconds.
The first 22 of its 26 methods report success. All 2,193 native unit cases
pass; only 2,213 of 2,228 declared native results execute before the stop.
The following 15 and extra tooling are not evidence. Eight other formal
green contexts do not qualify a later head.

This is the already retained original ten-second tool issue from #269.
Its earlier bounded local diagnosis observes 10.0749 seconds after 25 of
26 methods, then ordinary runs near 6.6-6.8 seconds. Current unchanged
fixture owners pass locally in 7.65174 and 7.9665 seconds. Earlier direct
diagnosis measures 3.5059 seconds in real verifier CLI waits. The exact
transient hosted delay remains unproven; no C++ engine is invoked by this
synthetic fixture owner.

The completed fixture and execution map is:

| Owner | Current responsibility |
|---|---|
| `ReadCompatibilityValidationTest.setUp` | One fresh temporary repository, pinned synthetic profile, corpus, oracle and canned runner per method. |
| `_write_runner` | Emit a Python canned producer; on Windows emit a `.cmd` wrapper which starts another Python process. |
| `_verify` | Start the actual verifier CLI as a real Python process with the unchanged arguments, pipes and result checks. |
| `read_compatibility._run_bounded` | Own one selected runner process, absolute deadline, pipe readiness, capture bounds, status and cleanup. |
| Existing timeout fixture | On Windows one native command process executes its built-in loop; no nested interpreter is needed. |

The real trace executable being modeled is one process. The Windows canned
producer adds an unnecessary nested interpreter behind that process owner.
This adds repeated startup to a bounded whole owner and makes its fixture
process shape differ from the single executable contract. No shared native
canned-response fixture already exists; use the current private emitter.

## Decision

Change only the Windows branch of the existing test `_write_runner`.
Parent-serialize each canned outcome with the existing JSON encoding and
newline semantics into an owned data file. Emit one `.cmd` producer which
selects the fixture's SQL line and uses native built-ins to copy the selected
literal response and an owned stderr file, then returns the requested status.

Keep payloads as file data, not interpolated command expressions. Preserve
literal bytes, quoted fixture-relative paths, stdout versus stderr and
explicit failures if a required response cannot be read. An unknown fixture
selection must report an error, not publish successful JSON.

Preserve the actual LF-only protocol. Native controls show `SET /P` retains
its lines in the first read, unlike a CRLF stream. Split that literal input
with the native deferred-expansion `FOR /F` path, then end deferred expansion
before interpreting fixture-relative filenames. Both LF/CRLF and paths with
spaces, Unicode and literal command metacharacters require actual controls;
do not normalize the production protocol to fit the fixture.

The Python source remains the unchanged POSIX producer. The Windows producer
must remain usable after that owned Python fixture source is retired. Adapt
the existing stderr-injection control to its Windows data owner while
retaining its assertion and the POSIX source-injection behavior.

Retain every actual verifier CLI invocation. Do not replace `_verify` with
in-process calls, mock the production capture or cache cases across methods.
All original 26 methods, fixture inputs, profile/hash/alias checks, normal and
mismatching transcripts, nonzero-child handling, stdout/stderr ceilings,
noninteractive launch without `shell`, absolute one-second child control,
eight-second traceback watchdog and whole-owner ten-second deadline remain.

No production Python tool, C++ engine, Windows VFS, selector, process policy,
CI environment, interpreter configuration, permission, timeout or workload
setting changes. This is a fixture ownership/startup correction, not proof
that a particular historical delay came from a particular startup operation.

## One bounded TDD source

After independent design acceptance, add one Windows-only actual CLI
regression: retire only the owned Python producer source, then require the
same supported and unsupported canned replay to succeed. Observe the old
wrapper's real missing-producer failure before changing the emitter.
Register that extra method only on Windows; retain the exact original
26-method population on POSIX and all original methods on Windows.
Cover both input line endings, literal paths, unknown selection and missing
owned response through actual capture, without adding an interpreter child.

Run the focused control, the complete Windows method population and the
original CTest owner. Verify actual original assertions, case identities,
capture limits, watchdog and deadlines, not merely a shortened elapsed time.
Exercise the existing stdout, stderr, status, mismatch, missing-case and
absolute-deadline controls through the actual capture and CLI. Confirm POSIX
behavior through its unchanged owner.

Run complete native fast units and applicable tooling. Reconcile unchanged
production, headers, compiler/policy blobs, binary hashes and selected cases
before reusing Source 2's production/sanitizer evidence under ADR-0102.
Do not call that reuse a new exact-head sanitizer execution.

This is one localized CI-gate repair after the two accepted metadata slices,
not a fifth source in #269 or a new SQL feature. Require SourceAccepted,
ledger evidence and one normal third commit to the same draft before final
exact-head compiler/native/sanitizer checks, one Full quality invocation,
bounded campaigns and integrated review. Preserve old failed runs, then
normally squash, qualify actual main and retire only delivered references.
Keep unrelated unmerged #269 at `8b714ea` on both ends.
