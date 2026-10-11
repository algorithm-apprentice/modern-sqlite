# ADR-0064: Production-Only Clang-Tidy Scope

- Status: Accepted
- Date: 2026-10-12

## Context

ADR-0046 made pull-request clang-tidy incremental and retained a full-tree
fallback on `main` or on pull requests labeled `full-quality`. The current
CMake wiring nevertheless enables clang-tidy on test, benchmark, fuzz-driver,
and command-line-tool targets in addition to the engine.

Six successful macOS `full-quality` jobs observed on 2026-10-11 took between
14 and 26 minutes. Most of that time was spent in the clang-tidy build rather
than setup. Test translation units account for a large and growing share of
the compilation database, while the same code is already compiled with
warnings as errors and exercised by Debug, Release, and sanitizer jobs.

The incremental helper also accepts every changed C++ translation unit found
in the compilation database. A test-only change can therefore spend pull
request quality time on clang-tidy even though the production engine is
unchanged.

## Decision

### 1. Analyze only production translation units rooted under `src/`

Clang-tidy applies only to C++ translation units whose normalized paths are
inside the repository's `src/` directory. Public headers under `include/` are
analyzed transitively through those production translation units.

Targets containing sources from `tests/`, `benchmarks/`, `tools/`, or any
other directory do not receive the `CXX_CLANG_TIDY` target property. This is a
positive production-source boundary rather than a list of excluded
directories.

Alternate engine targets whose complete source list remains under `src/`,
such as instrumentation, fuzz-engine, or diagnostic-engine variants, retain
clang-tidy when their configurations are enabled.

### 2. Apply the same boundary to incremental and full quality checks

`tools/run_changed_clang_tidy.py` selects changed `.cc`, `.cpp`, and `.cxx`
files only when their normalized paths are under `src/`. Its stable smoke
translation unit remains `src/base/bytes.cpp`.

The `full-quality` job builds the `modern_sqlite` and
`modern_sqlite_instrumentation` targets explicitly. Together they cover every
ordinary production translation unit without building the test suite or
command-line tools.

### 3. Keep test validation in the gates designed for tests

Test, benchmark, fuzz-driver, and tool sources continue to:

- compile with the repository's warning policy;
- run in their applicable Debug, Release, sanitizer, compatibility,
  performance, and fuzz jobs; and
- remain subject to formatting and engineering-discipline checks.

Removing clang-tidy from those sources does not skip test execution or weaken
production-source analysis.

### 4. Reserve pull-request full quality for broad production impact

Ordinary pull requests use changed-production-source clang-tidy and do not
receive the `full-quality` label by default. The label is reserved for shared
header changes, build or clang-tidy configuration changes, and broad
production refactors whose impact extends beyond directly changed
translation units.

`main` continues to run the production-only full-quality target after every
merge.

### 5. Isolate optional full quality from standard pull-request CI

Standard pull-request CI responds only to pull-request creation, reopening,
and new commits. Applying a label must not restart or cancel those standard
jobs.

A separate full-quality workflow responds to:

- every push to `main`;
- manual dispatch;
- application of the `full-quality` label; and
- new commits or reopening while that label remains present.

For a labeling event, the workflow runs only when the newly applied label is
`full-quality`. Its concurrency group is separate from standard CI, so
requesting the optional check cannot cancel an in-progress build, sanitizer,
incremental-quality, or performance job.

## Rejected alternatives

### Remove full-quality entirely

Rejected because changed-header and build-configuration effects can reach
unchanged production translation units that the incremental selector cannot
identify.

### Continue analyzing tests and increase the timeout

Rejected because it spends the longest quality gate on non-shipping code that
already has stronger execution coverage.

### Maintain an exclusion list

Rejected because each new non-production directory would have to be added
correctly. A single `src/` boundary is explicit and fails closed.

## Consequences

### Positive

- Clang-tidy cost grows with production code rather than test volume.
- Incremental and full checks share one understandable scope.
- New non-production targets are excluded without additional CI policy.
- Full production analysis remains available before merge and on `main`.
- Requesting full quality does not duplicate or cancel standard PR CI.

### Negative

- Clang-tidy-specific diagnostics in test, benchmark, fuzz-driver, and tool
  sources are no longer enforced.
- A mixed target containing production and non-production sources is excluded
  as a whole; production sources must also belong to a production-only target
  to receive analysis.

## References

- ADR-0005: Test-Driven Development
- ADR-0009: Build and Toolchain Baseline
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0049: Writable MVP Verification Harness
