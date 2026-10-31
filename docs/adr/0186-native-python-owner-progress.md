# ADR-0186: Native Python Owner Progress

- Status: Accepted
- Date: 2026-10-31
- Module graph node: `implement-advanced-sql`
- Prerequisites: accepted PR #278 content and ADR-0183
- Related contracts: ADR-0102, ADR-0106 and ADR-0178

## Actual-main blocker and completed diagnosis

PR #278 is normally squashed as `0236e43`, tree `f1dd1bb`, after two
SourceAccepted commits, actual final CI `41208531554`, the sole manual
quality union `41209563321` and IntegratedAccepted. Delivery ledger 141
records the squash, not completed actual-main qualification.

Fresh actual-main CI `41211367761` fails only native Windows. The existing
`differential.read_compatibility_tool` owner reaches its ten-second deadline
at 10.01 seconds. Its first 21 of 27 Python methods report success. All
2,196 native units pass; 2,216 of 2,231 declared native cases execute.
The following 15 cases and the separate tooling step are not evidence.
Eight other CI jobs and fresh whole-production quality `41211367849` pass.

The expired-preparation correction remains accepted, but its delivered
references must not be retired by claiming that failed main was green.
Preserve the failed run, PR #278's source tip `916437f`, and unmerged #269
at `8b714ea`.

One unchanged local CTest owner passes in 8.33 seconds; its own unittest
summary reports 5.695 seconds. A separate parent-only attribution run
preserves all 27 methods and seven real verifier CLI children, observing
about 2.92 seconds in those waits, 0.51 seconds in fixture setup and the
original absolute one-second child timeout.

The local Python 3.13 virtual environment differs from the hosted native
Python 3.12.10. The local startup gap does not establish a matched hosted
cause. A controlled canonical-module entry experiment retains the same
27 methods/seven children but measures 5.467 versus 5.475 total method
seconds. It provides no material improvement; do not ship that change.

Current ordinary logs identify completed method names, not the active
fixture, verifier wait or cleanup phase. The eight-second traceback timer
is armed after discovery/module loading; its absence at the outer cutoff
does not identify the missing time. A completed source/owner diagnosis
cannot honestly infer a particular scheduler, filesystem or startup cause
from those logs.

## Decision: repair diagnostic ownership, not deadlines

Add narrow progress records to this existing Python owning test module.
Use the standard unittest lifecycle and one private reporter. Do not add
an alternate test runner, process service, test sharding or CI wrapper.

### 1. Report actual owning lifecycle

The module's existing setup publishes a `module-ready` marker before
arming the unchanged traceback timer. Each actual test run starts one
wall/CPU interval and reports entry, fixture readiness, body completion,
cleanup entry/completion and test completion.

Use `time.perf_counter_ns` for elapsed wall time, `time.process_time_ns`
for this owner process's CPU time, and a Unix timestamp for correlation
with the external CTest start. Identify the actual process and test.
Owner CPU is not child CPU and module readiness is not process startup.
Never label completion as assertion success; standard unittest owns the
outcome. Failed bodies and cleanup still report their actual terminal
phase without swallowing or replacing errors.

Write compact, line-delimited records to the owning process's stderr
with an explicit prefix and flush. Do not create diagnostic files inside
fixtures or put records into captured runner/CLI stdout or stderr.
Logging errors remain visible; there is no silent fallback.

### 2. Report the same real verifier child boundary

Use one private launch helper for the existing `_verify` consumer and
the existing command-line usage control. Report immediately before the
unchanged `subprocess.run`, then after its actual return with the real
return code. A launch exception remains an exception, not synthetic
success or a fabricated `CompletedProcess`.

Retain all seven original real CLI invocations, exact argument lists,
working directories, pipes and result assertions. Do not switch to
in-process verification, canonical-module entry, a persistent interpreter,
different interpreter flags, process configuration or mock execution.
The native canned producer and POSIX producer remain unchanged.

## Two ordered TDD sources

1. Owning lifecycle records. First add an actual nested unittest failure
   control: the failure must remain recorded by unittest while progress
   reports the real entry and terminal phases. Observe the missing records
   on unchanged test implementation, then add the canonical reporter and
   standard lifecycle overrides. Preserve every original method and input.
2. Real CLI phase records. Strengthen the existing unknown-case control
   around its existing actual verifier invocation. Observe absent child
   phase records, then route both consumers through the same launch helper.
   Check the real nonzero status and unchanged child stderr separately
   from owner progress; do not add another CLI invocation.

Each source requires focused red/green, the full original owning selection
at its unchanged ten-second limit, complete fast-unit evidence, independent
SourceAccepted, a normal commit and append-only ledger before its successor.
Reuse unchanged C++/sanitizer evidence only after exact source, binary,
configuration, selection and input reconciliation. No claimed new execution
may be manufactured from that reuse.

Close one final exact-head actual CI/quality/platform/native/fuzz union and
integrated review before normal squash and fresh actual-main validation.
The prior failed `0236e43` run stays failed. If a later qualified descendant
closes the delivery blocker, prove its accepted lineage and unchanged
ancestor production content before retiring PR #278's delivered refs.
Do not pretend the descendant tree equals the earlier candidate tree.

This is diagnostic capability, not a measured startup optimization or proof
that transient timeouts cannot recur. If a new failure occurs, use its
actual phase/CPU records for the next bounded correction. Do not blindly
rerun the old attempt, raise the ten/eight/one-second limits, remove work,
weaken assertions or change environment/safety controls.

Node 43 and the four unexpired admission differences remain incomplete.
No unrelated SQL, storage, optimization or later DAG node starts here.
