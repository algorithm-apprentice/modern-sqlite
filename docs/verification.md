# Change-Aware Verification

Traceability: `implement-advanced-sql`, ADR-0005, ADR-0102.

## Choose work, not weaker assertions

The inner loop runs the focused regression and complete fast unit selection.
Local closing checks depend on affected mechanisms; isolated qualification
and required hosted checks remain separate.

Generate a plan from real staged, unstaged, deleted, renamed, and untracked
changes:

```sh
python3 tools/verify.py plan --base-ref origin/main
python3 tools/verify.py plan --base-ref origin/main --format json
```

A plan is not execution evidence. Its outstanding requirements include review,
hosted checks, and any compiler/native configuration that still needs to be
provided. Unknown changes and shared foundation/toolchain changes escalate
to full qualification. Mixed changes take the union of their domains.
Production changes also retain focused red/green or a green refactor baseline
as explicit evidence requirements. Project metadata, differential, CLI and
CMake harness changes select their own existing integration entry points.

Supply existing genuine GNU build directories when available:

```sh
python3 tools/verify.py plan --base-ref origin/main \
  --gnu-debug-directory build/gcc16-debug \
  --gnu-release-directory build/gcc16-release
```

Directory names do not prove compiler identity. Configure those directories
with the actual GNU compiler, and retain compiler/configuration evidence.
Use `--profile full` to request broader validation. There is no lower-risk
override for an unknown change.

| Change | Additional checks |
|---|---|
| Documentation | Review, references, ADR/graph consistency |
| Test/tool/harness | Own tests, negative controls, integration/discovery |
| SQL/compiler/VM | Native rows/types/errors, lifecycle/models, affected sanitizer/GNU; grammar fuzz where applicable |
| Codec/page/cursor | Goldens, malformed/OOM/ownership/pin/I/O, sanitizer/GNU, native interoperability, database fuzz |
| Durable write/recovery | Full qualification, fault/crash ordering, recovery, bidirectional images/integrity |
| Shared headers/base/cache/toolchain or unknown | Full relevant configuration matrix |
| Performance | Correctness/failure controls, fixed work and matched isolated timings |

Do not remove test inputs, failure cases, assertions, allocation ceilings,
thresholds, or deadlines to make a plan faster.

## Complete fast-local units

Build the development tests, then run:

```sh
cmake --build --preset dev-debug --parallel 2
python3 tools/run_fast_tests.py --build-directory build/dev-debug
```

The runner does not build or relink executables. Building only the ordinary
unit target leaves statically linked allocation/OOM runners stale; complete
the full development build above before treating its result as source evidence.

The runner reads CTest's actual `unit` selection. Only compatible ordinary
cases in `modern_sqlite_unit_tests` are grouped, at most 128 per process.
The discovered case deadlines are enforced through GoogleTest start/end
events and reported body time. Startup, inter-case, shutdown and output are
bounded. POSIX process groups and `ps` provide owned-child cleanup without
reaping/reusing the leader PID before group cleanup.

Standalone allocation/fault programs and tests with exceptional or unknown
properties remain isolated through CTest. Their timeouts, resource locks,
and expected-exit policies are not reimplemented by the batch runner.
Models, including the original ten-second sanitizer model, are not part of
the ordinary batch.

The result must account for every selected unit name exactly once, with no
failed, skipped, disabled, duplicate, or missing case. Failed batches are
not silently rerun in isolation to manufacture a pass.

The default report and per-batch XML/logs are under the build directory:

```text
build/dev-debug/verification/fast-local.json
```

The report says `process_mode: fast-local` and
`isolated_qualification: false`. Ordinary cases share a process, so this is
deliberately not the same isolation domain as:

```sh
ctest --preset dev-debug --no-tests=error
```

Keep isolated presets and required hosted gates. Never substitute a fast-local
report for an ASan/GNU isolated qualification result or the original model.
One report path is locked for the invocation. Reports carry a run identifier,
normalized discovery/deadlines, source/environment and all executable/input
identities checked before and after execution. Partial outcomes remain in
failed reports; concurrent writers cannot replace another invocation's result.

## Share configuration-independent checks

The canonical policy is `project/verification-policy.json`.
Its Python/tool/native-oracle tests have the additional
`configuration-independent` label. All old labels, names, commands,
assertions, timeouts, and conditional registrations are retained.

Within one unchanged-input local plan:

```sh
python3 tools/verify.py run-shared \
  --owner-build-directory build/dev-debug \
  --build-directory build/asan-ubsan \
  --build-directory build/benchmark \
  --report build/dev-debug/verification/shared.json
ctest --test-dir build/dev-debug -LE 'unit|configuration-independent' \
  --output-on-failure --no-tests=error
```

List every configured tree whose later selection excludes the shared label.
The shared command proves the owner is a superset and each excluded command,
runtime property, timeout, interpreter, directory and environment is identical.
An optional oracle present only in another tree, or a different interpreter,
hard-fails before execution. Configure a matching owner or retain the original
unfiltered CTest selection; do not silently omit that test.

The complete local coverage is the union of shared checks, complete fast
units, and the applicable remaining selections. Other compiler/runtime
selections exclude the shared label only after that plan's shared step.
Hosted CTest commands remain unchanged and retain their normal selections.

There is no automatic cross-invocation cache. Reusing existing successful
evidence requires matching source/input, binary, compiler/tool, configuration,
environment, process mode, selection, seed, and timeout identity.
Changed, failed, skipped, or incomplete evidence is not reusable.

## Target native builds

Configure the existing pinned Release benchmark preset, then use:

```sh
python3 tools/verify.py build-native --build-directory build/benchmark --parallel 2
```

This uses the same canonical target list as CI, not the default ALL target.
It avoids unrelated unit/model/allocation links. It rejects non-Release,
unconfigured, differently rooted, sanitizer, and instrumented timing builds.

The ordinary timing library and the instrumented diagnostic engine remain
distinct. Run native timings without concurrent builds or fuzzing.
The native CTest selection remains `performance|compatibility`; a local plan
may exclude its already completed shared checks, with exact union accounting.
No benchmark corpus, configuration, threshold, or timing assertion changed.

Full/durability plans retain the existing bounded read and write SQL/database
fuzz targets, with writable generated corpora outside repository seeds.
