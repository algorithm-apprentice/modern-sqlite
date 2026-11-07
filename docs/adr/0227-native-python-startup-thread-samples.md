# ADR-0227: Native Python Startup Thread Samples

- Status: Accepted
- Date: 2026-11-07
- Module graph node: `implement-advanced-sql`
- Blocked ancestor: PR #304, main `fd633156`, tree `3310cdce`, ledger276
- Related contracts: ADR-0102/0106, ADR-0194/0195/0197/0209/0210/0226

## Observed gap

Original main CI42370197257 attempt1, Windows job128396541274, passed all2306
native units and2325 cases before `differential.read_compatibility_tool`
timed out at10.19s. It never emitted module readiness or entered a method.
Fifteen subsequent cases and the separate tooling/backend steps did not run.
The failed run, annotations and complete raw archive remain retained.

The reporter-excluded `_hashlib` execution interval is5.2230934s. SSL creation
and execution take1.2637745s and0.2997088s. Each reports zero additional owner
CPU at that clock's resolution. Those intervals do not distinguish a waiting
thread from a runnable thread not being scheduled, or identify a native
function, provider, security product, filesystem or OS cause.

The same tracked premerge tree passes39 methods in3.25 functional seconds,
reaching readiness after0.7911946s. One current-source local diagnostic with
the existing Python3.12.10 and CTest4.4.5 passes39 methods, with0.3194674s
entry-to-ready,6.78877s functional and7.436808s outer wall time. It does not
reproduce the hosted stall. Matching versions does not prove equal hosts,
binaries or cache states. Automatic main quality's successful metadata is
not replacement Windows evidence.

More completed-import boundaries cannot recover the missing thread state.
CPython3.12's existing traceback watchdog reports Python frames, not a native
wait stack. Windows wait-chain traversal requires a debug privilege and does
not support every wait primitive; this decision does not enable that
privilege. No privileged tracer, debugger attachment, dump, memory inspection,
module replacement or broad host-process collection is introduced.

## Decision

### Observe the existing owner, not a replacement execution

Add one Windows-only PowerShell pipeline observer around the existing native
correctness step. Continue executing the original CTest preset exactly once,
adding only `--verbose` so existing flushed owner records reach the pipeline
while the process is alive. Forward all received CTest output unchanged.
Preserve the native CTest exit code; diagnostic failure must be explicit and
cannot turn a failing CTest result into success.

The first canonical module-level `bootstrap-ready` record identifies the
Python PID. Start at most one bounded sampler using the already running
PowerShell installation. Before collecting thread information, verify that
the live Python process is a direct child of the CTest process and that CTest
is a direct child of this workflow shell. Record the observed creation
identities and retain the target process object; reject an inconsistent or
changed identity. Never discover targets by a system-wide name match, follow
unrelated processes or inspect command-line/environment contents.

The sampler reads only that process's ordinary .NET process/thread metadata:
process identity, thread IDs, thread state, wait reason when state permits,
and accumulated thread/process CPU time. Include UTC timestamps, monotonic
sample intervals and observation duration. A thread-state race or an exited
target is reported distinctly; it is not fabricated as a waiting sample.
Other query, launch, decoding or reporting errors remain explicit failures.

Sample no faster than every250ms, for at most ten seconds after sampler
entry. The first matching `module-ready` record requests sampler termination;
later readiness records cannot restart it. Owner exit also ends sampling.
Samples are correlated with the original owner timestamps, not assumed to
precede readiness merely because output delivery was delayed.

Read sampler stdout/stderr asynchronously into bounded output, not an
unconsumed pipe. Each stream has one non-resizable1MiB byte-array-backed
memory stream and fixed-size asynchronous copy chunks. An overproducing
stream fails during collection, before EOF; a post-EOF size check is not
the allocation bound. Decode only written bytes using strict UTF-8.
A single terminal cleanup path requests stop, waits at most
two seconds, and terminates only the owned sampler process if necessary.
Unexpected forced termination is a diagnostic failure. Cleanup also applies
to malformed progress, pipeline errors, earlier CTest failure and cancellation.
Do not terminate the owner or any other process from the observer.

A fast startup may finish before the sampler initializes. Report that case
as no startup sample, not as success evidence for a waiting/runnable claim.
Likewise, sparse ready/wait samples and observer scheduling gaps are
observations, not proof of continuous starvation, a deadlock or an OS cause.
No native call-stack or security-product attribution is claimed.

### Keep all test work and limits

The Python owner, canonical progress reporter, import order, interpreter
selection, `-X importtime`, `-m unittest -v`, directories, two processor slots,
all39 methods and actual CLI children remain unchanged. Preserve startup10,
functional10, traceback8 and child1 limits, every other test deadline and the
25-minute job limit. Do not warm imports, read DLLs, select providers, change
security settings, rerun a suite or move its work outside CTest.

The sampler is a diagnostic peer, not another test supervisor or a worker
that runs test work. Its lifetime limit and cleanup allowance do not extend
CTest's owner deadline. The additional process and verbose output have
observable overhead; do not claim identical scheduling or a latency fix.
All non-Windows commands and existing always-run progress publication remain
unchanged. No production C++ input changes.

This narrowly supersedes the earlier prohibition on an additional diagnostic
process/pipeline for these bounded samples only. It does not authorize a
generic monitoring framework, a new crypto/import implementation, a
source-neutral retry, or further SQL implementation while delivery is blocked.

## Test-first, review and delivery

Before implementation, add focused controls through the existing workflow
policy owner. Use real, owned CTest/Python fixture processes to observe a
sleeping owner and an executing owner, without asserting a particular
scheduler outcome for a short sample. Require valid PID/parent/creation
binding, nonnegative clocks and CPU measurements, and complete cleanup.

Exercise first-readiness stop, repeated readiness, early owner exit, nonzero
CTest propagation, malformed or foreign-PID progress, sampler failure, and
bounded termination. Reuse shared fixtures rather than building a second
test runner. A genuine red must establish the missing observer behavior.
Run Windows owning and workflow-policy checks at their original deadlines,
the complete native correctness selection with the actual observer pipeline,
and selected non-Windows tooling/graph/discipline checks. Reconcile unchanged
C++ source/configuration/image/fast evidence explicitly under ADR-0102.

Review the bounded implementation and its actual evidence independently
before a normal source commit and ledger. A corrective PR descends from
`fd633156`; it must preserve every PR #304 production/test input except the
explicitly named diagnostic workflow/policy changes and new observer.
Authenticate the actual final-head local/hosted/integrated/squash/main gates.
No failed original run is retried or rewritten.

Qualified-descendant closure requires separately reviewed proof of ancestry,
unchanged ancestor engine/owning tests, actual required descendant bodies,
all relevant annotations and expected branch OIDs. Only that authorization
can close PR #304's delivery block and retire its retained source references.
The original main failure remains failed and causally unresolved unless new
evidence actually explains it. Preserve all three stashes. General generation,
IN and node43/project completion remain separate and incomplete.

## References

- [Python3.12 faulthandler](https://docs.python.org/3.12/library/faulthandler.html)
- [Windows wait-chain constraints](https://learn.microsoft.com/en-us/windows/win32/api/wct/nf-wct-getthreadwaitchain)
- [.NET process thread state](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.processthread.threadstate)
- [.NET thread wait reason](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.processthread.waitreason)

## Owning-test budget correction

- Amendment status: Accepted

The first native `tools.ci_workflow_policy` execution after adding the
diagnostic integration controls timed out at its actual unchanged10s limit,
10.05s observed. The earlier assumption that this owner had30s was incorrect.
The original log, XML and discovery are retained. The new focused controls
alone already consumed10.595s before additional boundary cases; this is
introduced test workload, not a reproduction of the hosted import stall.

Place the new diagnostic controls in their own Windows-only tooling module
and register four isolated10s CTest owners: live samples, progress protocol,
owner/sampler exit propagation, and ownership/bounded cleanup. Keep one
shared real-CTest fixture implementation. Do not copy or split existing
workflow-policy methods, change its deadline, omit a new control, or reduce
its sleeping/executing/forced-termination work to manufacture a pass.
Use two processor slots for each new owner and retain serial native tooling
execution. Extend the existing native tooling step to execute all four in
addition to the original verification and workflow-policy owners.

Only these new Windows contracts acquire new registration; other platforms
and all existing test commands, populations, reservations and deadlines
remain unchanged. The general workflow-policy owner checks their real
registration/selection and unchanged existing declarations. Authenticate
the four actual native bodies separately. No success is inferred from
source routing or from the failed original combined-owner execution.

This amendment changes only the test ownership specified above, and requires
independent acceptance before implementation. The underlying read-only
observer design and all failed-main/descendant-delivery gates remain binding.

## Configured interpreter launch ownership

- Launcher amendment status: Accepted

The first six-owner native run passes the new protocol, exit and ownership
contracts but rejects the live-sample target. Earlier focused controls used
the shell's direct Python executable; the canonical native CTest cache instead
selects an existing MSVC Python3.13.13 virtual environment.

A separate bounded metadata observation confirms that its `Scripts\python.exe`
is a redirector: the marker PID belongs to `python3.13.exe`, whose parent is
the configured `python.exe` launcher. This matches the explicit child creation
in [CPython3.13.13's Windows venv launcher](https://github.com/python/cpython/blob/v3.13.13/PC/venvlauncher.c).
It is not the hosted Python3.12 startup stall, and neither interpreter is
substituted. The failed native observation remains failed.

Bind the sampler to the exact interpreter path already selected in the native
CMake cache, read as metadata without running Python or reading an executable.
Accept either that image directly under CTest, or the marker process directly
under that configured launcher, with the launcher directly under CTest.
CTest must still belong to the current workflow shell. Require ordered
creation identities and exact normalized configured-image path equality;
do not use a filename wildcard, arbitrary ancestor walk, environment search
or a system-wide process-name match as ownership proof.

Capture only the marker process's threads, never the redirector's waiting
thread. Report the actual image, configured-image PID and optional redirector
relationship. Sparse samples still do not identify a native stack or the
importing thread by array position. Missing, mismatched or inaccessible live
identity remains an explicit diagnostic failure; an already exited target
remains an explicit unavailable observation.

Keep the existing owner module and its imports/records unchanged. Extend the
same real-CTest control to verify the configured-image binding and retain the
foreign-PID rejection. This amendment requires independent acceptance before
implementation; it changes no deadline, workload, privilege or delivery gate.
