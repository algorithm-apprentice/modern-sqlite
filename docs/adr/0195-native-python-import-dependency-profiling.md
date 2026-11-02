# ADR-0195: Native Python Import Dependency Profiling

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0102, ADR-0106, ADR-0186 and ADR-0194

## Observed actual-main blocker

PR #285 has three accepted sources, final CI41432475387, the sole manual
quality41434013884 and IntegratedAccepted180. Normal squash181 produces
`cabadbd2`, tree `7e1aed73`, without changing the reviewed source tree.
Its actual-main CI41437428164 nevertheless times out the original10s native
Python owner at10.03s. All2217 native units pass, but no Python test method
begins. The following15 native cases and separate tooling step are not
execution evidence. The same-tree premerge owner passed3.45s.

The ADR-0194 records place module entry76.6ms after CTest start. The
faulthandler-ready to hashlib-ready boundaries span7.4238269s, with a reported
owner-CPU delta0 subject to clock resolution. The final completed boundary is
unittest-ready; the import of unittest.mock does not reach stdlib-ready.
These intervals include reporter overhead. They do not prove a DLL,
filesystem, security-software or scheduler cause.

A separately installed, signature-verified official CPython3.12.10 runs the
unchanged owner locally in6.40144s at the original10s deadline, preserving all29
methods and seven real CLI calls. Only that diagnostic instance's interpreter
path differs; the repository preset and cache are unchanged. Its hashlib and
mock boundaries take about46.9ms and423.5ms. Matching the version does not make
the local machine, distribution or cache state equivalent to the hosted runner.

Parent-only import profiling identifies the local OpenSSL-backed `_hashlib`
path and the asyncio/`_ssl` dependency of unittest.mock. A separate controlled
full-owner invocation with only the profiling option added also preserves all29
methods/seven CLI calls and the10s limit. Neither observation establishes a
hosted cause or a performance improvement. The failed attempts remain failed.

## Decision

Use CPython's existing `-X importtime` diagnostic instead of another importer,
audit/profile hook, worker, watchdog, crypto implementation, warmup or runner.
It reports nested import self/cumulative timings to the owner's stderr.
CTest retains that output with the existing lifecycle markers.

Add these two arguments only to the existing
`differential.read_compatibility_tool` CTest command when `WIN32` is true.
Initialize one private argument-list variable empty, assign it only in that
condition, expand it only into this command, and unset it afterward. Add no
cache option or environment variable.

Keep the same interpreter executable, `-m unittest -v` entry, test path,
working directory, labels and10s deadline. The non-Windows command and every
other native owner command remain unchanged. Do not propagate the profiler
option into the seven real CLI children or producer commands.

This narrowly supersedes ADR-0186/0194's unchanged-interpreter-flags clause
for this diagnostic option only. Their workload, ownership, failure-reporting
and10/8/1-limit requirements remain. The Python test module, imports/order,
reporter/clocks,29 methods, original assertions and no-suite/no-timer import
control are unchanged. No interpreter version, environment, crypto provider
or security policy is changed.

This is import-dependency observability, not a latency fix. A successful
profiled run must not be used to attribute a historical failure or to claim
that future host stalls cannot recur.

## Test-first and delivery

Add one integration control in the existing workflow-policy test owner.
Configure the actual adjacent CLI/read-compatibility CMake declarations in a
small `project(NONE)` fixture with their imported read-trace target and real
Python/source variables. Exercise `WIN32` true and false as declaration-only
controls, using the existing shared-label policy helper.

Inspect actual CTest JSON: the same two owners exist; only the Windows
differential owner gains the two diagnostic arguments; the unaffected CLI,
original arguments, labels, working directory and10s deadlines survive.
Observe the genuine missing-flag failure before editing production CMake.
Do not claim simulated declaration controls are native Windows execution.

Run focused green, the real native owning suite and workflow-policy owner at
their original limits, non-Windows discovery and the selected tooling plan.
Reconcile unchanged C++ sources, actual compile configuration, image hashes
and complete fast evidence; verify the one intentional discovery delta
exactly rather than claiming wholesale command identity.

Require independent exact-source review and a normal corrective PR from
`cabadbd2`. Preserve PR #285's delivered source references until a qualified
descendant proves accepted lineage and unchanged ancestor production.
Fresh exact-head hosted/integrated/squash/main gates remain mandatory. Never
claim a changed descendant tree is `cabadbd2`'s tree, retry the failed run to
manufacture a pass, or begin another production prerequisite while this
actual-main blocker remains unresolved.

The profiler is documented in the
[CPython command-line reference](https://docs.python.org/3.12/using/cmdline.html#cmdoption-X).
