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

## Native Windows

ADR-0153 admits the ordinary desktop Win32 VFS through the same storage,
compiler and session implementations. The first supported compiler is native
MSVC/VC++ from Visual Studio 2026. Open an x64 developer shell with native
CMake, Ninja and Python available. The preset explicitly selects `cl` and
owns `build\windows-msvc-debug`; never reuse an experimental MinGW cache as
MSVC evidence. WSL results remain Linux evidence.
Evidence identity checks read the interpreter image. Use a normal native
Python executable or a native virtual environment, rather than an unreadable
Windows Store execution alias; select it with `Python3_EXECUTABLE` when needed.

```powershell
cmake --preset windows-debug `
  -DMODERN_SQLITE_PINNED_SQLITE_C="C:\sqlite-build\sqlite3.c" `
  -DMODERN_SQLITE_PINNED_SQLITE_H="C:\sqlite-build\sqlite3.h"
cmake --build --preset windows-debug --parallel 2
ctest --preset windows-debug
ctest --test-dir build\windows-msvc-debug --output-on-failure --no-tests=error `
  -R '^tools\.(verification|ci_workflow_policy)$'
```

Use the exact version, Fossil check-in and generated-source SHA-256 pins in
`project/manifest.json` and `cmake/PinnedSqlite.cmake`. The canonical CMake
correctness build supplies its own pinned MSVC SQLite shared library; a
system SQLite DLL or a WSL library is not an equivalent oracle. The Windows
CI job uses the Visual Studio 2026 runner image, generates the source with
native `nmake` and the snapshot's bundled Jim Tcl, and checks both source
hashes before building. Its normalized Tcl script path and narrowly staged
source verifier preserve the original manifest attestation; do not edit
generated comments or pins to compensate for a different invocation.

The live oracle tests explicitly select `--sqlite-compiler-profile msvc`.
The unchanged SQLite source reports `ATOMIC_INTRINSICS=0` for MSVC and `1`
for GCC/Clang. Both profiles require their exact declared option and preserve
every other source/configuration check. Generated native metadata retains
the actual `0`; regeneration adapts only that expected compiler field while
comparing every other canonical golden byte unchanged. The committed goldens
and historical performance archives retain their original GCC/Clang profile.

The MSVC profile keeps the Debug CRT and project assertions, with
`_ITERATOR_DEBUG_LEVEL=0` and `_MSVC_STL_HARDENING=1`. Intrusive iterator
bookkeeping allocates inside vendor `noexcept` constructors and cannot
satisfy the typed OOM contract. The selected ABI enables non-intrusive STL
precondition checks, not exhaustive iterator-invalidation tracking.
All C++ consumers, including GoogleTest, must share this ABI; link the public
CMake target to inherit its definitions. Bootstrap controls check the ABI
and hardening, and the full unchanged allocator sweeps remain mandatory.

The preset selects complete isolated `unit` coverage and the `platform`,
`model`, `compatibility`, `differential`, `cli`, `project` and `fuzz` labels.
It does not claim the POSIX-only fast-local runner, POSIX performance child
harnesses, other Windows compiler families or sanitizer configurations ran.
Keep ordinary process stacks, existing limits, assertions and deadlines.
The CLI byte-stream protocol is qualified; Unicode command-line argument
conversion is not admitted by the backend's separate UTF-8 filename tests.

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

Committed performance archives use `validate-historical-baseline`: recorded
source-code fingerprints remain historical metadata, while workload, profile,
fixture and raw-artifact checks remain strict. `validate-baseline` still checks
current inputs by default. Neither archive validation nor archived timings
replace fresh matched evidence for a changed engine.
When only the oracle generator fingerprint changes, historical validation
authenticates the recorded profile against its byte-exact, content-addressed
snapshot in `tests/compatibility/profile-history`. Every other profile field,
including the generator path, must still equal the current profile. A missing
or modified snapshot fails; historical archives are never rewritten.

ADR-0149 adds a bounded SELECT frontend family without adding a database or
compatibility corpus. `parse` measures complete syntax-owner construction and
cleanup, using an explicitly derived native copy with two guarded semantic
callbacks; `prepare` uses the unchanged native public API and Modern session
facade on the existing ordering fixture. The private native APIs cannot
interpose public prepare. Neither phase executes the prepared statement.

After the canonical native build, generate nine isolated samples per engine
and phase from a clean reviewed source revision:

```sh
python3 tools/preparation_performance.py generate \
  --output build/evidence/preparation-baseline-v1.json
python3 tools/preparation_performance.py validate-baseline \
  --baseline build/evidence/preparation-baseline-v1.json
```

The single archive retains exact SQL, raw reports, phase-specific work and
lifecycles, separate allocation diagnostics, source/build identities and the
existing alternating three-round schedule. These measurements do not
automatically approve the parser alternative or imply full-grammar parity.

Full/durability plans retain the existing bounded read and write SQL/database
fuzz targets, with writable generated corpora outside repository seeds.

ADR-0152's scalar-list IN family reuses the ordinary and diagnostic ORDER BY
binaries as typed-row consumers, without adding an executor or sorter.
Its five cases retain the fixed 65,536-row fixture, zero native sorts,
eight measured executions, nine samples per engine and the declared 10/1
CPU/wall guards. Validate the portable source-qualified archive with:

```sh
python3 tools/in_performance.py validate-historical-baseline \
  --repository-root . \
  --workloads tests/performance/in-workloads-v1.json \
  --baseline benchmarks/in-baseline-v1
```

Use the same driver's `generate-baseline --help` for fresh isolated paired
measurements after a clean native build. Archived timing/source identities
remain historical evidence, not proof that a different head was executed.
