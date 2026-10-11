# ADR-0065: Production-Only Sanitizer Instrumentation

- Status: Accepted
- Date: 2026-10-12

## Context

The ASan/UBSan preset currently attaches sanitizer compile and link options to
the `modern_sqlite_project_options` interface target. Every target that links
the production library therefore inherits sanitizer compile instrumentation,
including test and command-line-tool translation units.

The successful sanitizer job for pull request 175 took 6 minutes 15 seconds:

- dependency setup and configuration took 18 seconds;
- compilation took 3 minutes 1 second; and
- test execution took 2 minutes 49 seconds.

The generated Ninja graph contained 131 sanitizer-instrumented C++ compile
commands. Only 40 compiled production sources under `src/`; 89 compiled test
sources and 2 compiled tool sources.

ASan and UBSan find production defects when tests execute instrumented engine
code. Test translation units must continue to provide that workload, and their
executables must link the sanitizer runtime, but the test source itself does
not need compile instrumentation to exercise an instrumented static library.

## Decision

### 1. Instrument concrete production libraries

Sanitizer compile options apply directly to the concrete production libraries
whose reviewed source lists are rooted under `src/`:

- `modern_sqlite`;
- `modern_sqlite_instrumentation`;
- `modern_sqlite_fuzz_engine`; and
- `modern_sqlite_diagnostic_engine`.

Configuration-specific targets retain their existing availability rules. The
fuzz and diagnostic variants receive sanitizer options only when their
existing configurations enable them.

`modern_sqlite_project_options` continues to propagate the C++ language
standard, but no longer propagates sanitizer compile options.

### 2. Separate compile instrumentation from runtime linkage

`modern_sqlite_enable_sanitizers()` applies:

- `-fsanitize=...`, `-fno-omit-frame-pointer`, and
  `-fno-sanitize-recover=all` as private compile options on the production
  library; and
- `-fsanitize=...` as an interface link option for executables that consume
  the instrumented static library.

Tests and tools therefore compile without sanitizer instrumentation while
their final executables still link the required runtime whenever they consume
an instrumented production library.

### 3. Keep the complete sanitizer workload

The `asan-ubsan` build and test presets remain unchanged. CI still:

- builds the complete ordinary target graph;
- executes the complete required CTest preset;
- treats sanitizer findings as nonrecoverable failures; and
- retains frame pointers for actionable diagnostics.

This decision changes instrumentation scope, not test selection.

### 4. Lock the propagation contract with a CMake fixture

A focused CMake contract verifies that:

- a concrete production library receives private sanitizer compile options;
- those compile options do not appear in its interface;
- consumers inherit sanitizer runtime link options; and
- a test consumer does not receive sanitizer compile options from the
  production library.

Repository validation also checks the generated Ninja commands: sanitizer
compile instrumentation must be present for `src/` translation units and
absent from test and tool translation units.

### 5. Keep documentation-only workflow filtering separate

Skipping expensive workflows for Markdown-only changes is a distinct trigger
policy and will be delivered in a separate small pull request. This change
does not alter workflow events or path filters.

## Rejected alternatives

### Remove the sanitizer pull-request job

Rejected because memory safety and undefined-behavior failures must be caught
before merge rather than after they reach `main`.

### Skip sanitizer test execution

Rejected because instrumented production code reports defects only when a
workload executes the relevant paths.

### Add `-fno-sanitize=all` to every test target

Rejected because a negative override depends on compiler-option ordering and
would require every new non-production target to remember the override.

### Continue instrumenting all test and tool sources

Rejected because the additional instrumentation increases build and runtime
cost without improving coverage of shipping translation units.

## Consequences

### Positive

- Sanitizer compile cost scales with production code rather than test volume.
- Every existing sanitizer test continues to execute instrumented engine code.
- Runtime linkage remains automatic for consumers of sanitized static
  libraries.
- New test and tool targets are uninstrumented by default.

### Negative

- ASan- and UBSan-specific diagnostics are no longer guaranteed for defects
  located only in test or tool source code.
- Production targets must call the sanitizer helper explicitly when they are
  introduced.

## References

- ADR-0005: Test-Driven Development
- ADR-0009: Build and Toolchain Baseline
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0049: Writable MVP Verification Harness
- ADR-0064: Production-Only Clang-Tidy Scope
