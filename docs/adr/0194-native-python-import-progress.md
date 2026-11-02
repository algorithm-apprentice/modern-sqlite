# ADR-0194: Native Python Import-Phase Progress

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Delivery: bounded qualification follow-up in PR #285
- Related contracts: ADR-0102, ADR-0106 and ADR-0186

## Observed blocker, not an inferred cause

Source `3ac5743`, tree `7eaccf7c`, has accepted design/source, genuine runtime
red, complete local native/sanitizer/fast evidence and56 paired native read
controls. Actual CI41420947752 nevertheless fails its native Windows owner
`differential.read_compatibility_tool` at10.01 seconds under the original10s
deadline. All2217 native units pass;2237 of2252 native cases execute, and the
following15 plus the separate tooling step are not execution evidence.
Eight other contexts pass, including the explicitly nonexecuting PR contexts.

The existing phase records place module readiness8.4435 seconds after the
external CTest start. Only17 methods finish before the cutoff; the18th reaches
fixture-start. Module readiness is after imports and unittest loading, so
this interval does not establish interpreter, scheduler, filesystem, import,
security-software or C++ causation.

The Python owner, verifier, registration and workflow are unchanged from the
qualified parent. One unchanged local owner passes7.97s, with28 platform-
discovered methods and5.389 unittest seconds. Six CLI boundaries are visible;
the seventh remains intentionally captured and asserted by its passing
unknown-case control. Static top-level AST counting misses the Windows-only
method and must not replace actual platform discovery.

An import-only local diagnostic takes0.450s, with0.308s accumulated for the
owning module. Local Python3.13.13 is not hosted Python3.12.10, so this is not
a matched hosted explanation. No import optimization, different interpreter,
canonical-module entry or deadline change is justified by these observations.
The failed run remains failed and is not blindly rerun.

## Decision

Extend the existing owning module's single progress reporter to cover the
currently unobserved import/discovery boundary. Do not introduce another
runner, process service, generic logger, CI wrapper or parallel serializer.

Immediately after importing the existing standard `time` module, capture
module-entry Unix, monotonic-wall and owner-CPU values. This is entry into the
test module, not process startup. Load the minimal existing JSON/OS/stderr
dependencies, define the same canonical reporter, and emit `bootstrap-ready`
against that captured interval.

Retain the remaining original imports and emit `stdlib-ready` after the
standard-library imports and `harness-ready` after the existing compatibility
harness import. Both use the same captured wall/CPU origin. Module-scoped
records additionally retain the captured module-entry Unix timestamp so a
future failure can distinguish the pre-module interval from bootstrap,
standard-library import, harness import and subsequent unittest loading.
Do not label unobserved time as a proven cause.

Keep the existing `module-ready` point in `setUpModule`, all actual test/
fixture/body/cleanup records and the seven real CLI boundaries. The reporter
continues writing only flushed owner stderr; logging/import exceptions remain
visible. Captured producer/CLI stdout and stderr are not contaminated.

The exact CTest entry, interpreter flags, command, working directory, inputs,
producer behavior and10/8/1-second owner/traceback/child limits remain unchanged.
No environment or security controls are modified, no work is skipped and no
warmup or alternate entry path is shipped. This is diagnostic ownership, not
a performance fix or proof that transient host timeouts cannot recur.

## Test-first and qualification

Add one focused owning test that imports this module under an isolated name
without running its suite or arming its traceback timer. Observe the missing
bootstrap/import records on unchanged implementation before adding them.
Then require the ordered three records, one captured entry identity, real
owner PID and bounded nonnegative wall/CPU intervals through the existing
reporter. Preserve every prior assertion and method; the native population
becomes29 with the same seven real CLI invocations.

Execute the full owning selection at its unchanged10s CTest deadline. Parent
reconciles all reused C++/sanitizer evidence against unchanged source inputs,
binary hashes, configurations, selections and deadlines under ADR-0102.
Do not claim new C++ execution from reuse. Run changed Python/graph/discipline
checks and obtain independent source review before a normal follow-up commit.

Update the same draft after acceptance. The final union still requires fresh
exact-head native/compiler/runtime/shared/fuzz bodies, integrated review,
normal squash, actual-main qualification and reference cleanup. Source177's
failed hosted attempt is not rewritten or treated as passing; its accepted
production content remains unchanged. Further SQL prerequisites remain held.

## Standard-library boundary follow-up

Source `7d92ae68`, tree `44b00dfe`, supplies the accepted import observations.
Its actual CI41429139727 still times out the same owner at10.19 seconds.
All2217 native units pass, but no Python test method begins. Module entry is
about70 milliseconds after the external CTest start, and `bootstrap-ready`
follows about2 milliseconds later. No `stdlib-ready` record is observed.
This narrows the new failure to the interval covering the remaining standard
imports and the next reporter call; it does not identify a particular module,
filesystem, security product or scheduler as the cause. The earlier failed
run's unobserved interval is not retroactively attributed.

Refine that existing interval with a completed boundary after each original
import of `faulthandler`, `hashlib`, `io`, `pathlib`, `subprocess`, `tempfile`
and `unittest`. The existing `stdlib-ready` boundary remains immediately
after `unittest.mock`. Use the same reporter and original module clocks.
Do not wrap, replace, defer or reorder imports, add another importer or timer,
or change the interpreter, environment, execution entry or10/8/1 limits.
The last completed boundary must not be presented as proof that the next
module is itself defective.

Extend the existing isolated-import observation test to require these ordered
boundaries before adding them. Keep its no-suite/no-timer assertions and
every other method unchanged; the native population remains29 and the seven
real CLI calls remain. Repeat the focused/full owning checks, source-matched
fast reconciliation and selected tooling plan before independent source
review and a normal follow-up commit. Both failed hosted attempts remain
failed; final exact-head gates, integrated review and actual-main closure
are still required. This is diagnostic coverage, not a timing fix.
