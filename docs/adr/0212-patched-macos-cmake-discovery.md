# ADR-0212: Patched macOS CMake Discovery

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source224 `841f7d6`, ledger224, draft PR #294
- Related contracts: ADR-0005/0009, ADR-0102/0106 and ADR-0210/0211

## Observed failure and completed investigation

Source224 completes the original full34 local plan, including all four
20000-run, seed102 campaigns. Supplemental41903485868 executes and passes
all four jobs: actual Intel full-production LLVM22 analysis, platform
regression and both bounded fuzz families. Preserve these executions with
their original source identities.

Standard CI41903485476 fails both Intel Debug/Release jobs during discovery
of `modern_sqlite_unit_tests`, before any CTest case executes. The generated
command invokes CMake's `GoogleTest/DiscoverTests.cmake` and reaches the
default five-second discovery deadline. This is not a unit-test assertion,
the native Python owner, or a failure to acquire a runner. Windows completes
19/2288/2 cases, with the Python functional phase taking3.12 seconds.

Both failed jobs and the passing source223 jobs report image
`macos-26/20260824.0517.1`. Its published inventory lists CMake4.4.2. The failed
jobs do not print `cmake --version` or a module digest, so that inventory is
not an independently captured executable version. Their generated discovery
command matches the affected upstream implementation.

CMake4.4.2 implements PRE_TEST discovery through a new CTest command which
bounds the whole helper process, including test-list parsing. It also wraps
individual cases in a CMake launcher and rediscovers unchanged executables.
Upstream commit `5e5467bfc3b5e5a4773a20a442bd36b6c5ff8480` reverts that
implementation because it does not persist discovery and can substantially
increase overhead. The official4.4.5 release contains the correction.
Its existing five-second limit bounds the executable's enumeration; list
processing and cached discovery retain the upstream mechanism.

Matched local diagnostic projects use the same actual MSVC test executable
and official authenticated CMake4.4.2/4.4.5 packages. Both discover the same
2086 unique cases with ten-second case limits. Direct enumeration takes
less than0.2 seconds. Both versions complete within their discovery limit
locally; this is not reproduction of the hosted timeout or fresh macOS
qualification. Generated commands confirm the old wrapper versus corrected
direct executable invocation. The corrected implementation retains generated
test definitions for reuse. Do not describe these observations as an engine
speedup or proof of the unique cause of the hosted delay.

## Decision

Use the official prebuilt CMake4.4.5 macOS universal release for every
existing macOS CMake consumer in both workflows. Keep `macos-26-intel`,
the existing x86_64/Bash/pipefail guards, all other tools and action pins,
and the architecture-owned compiler caches.

One narrow Python bootstrap owns the fixed release version, HTTPS URL,
archive byte count and SHA256. The release archive SHA256 is
`679e77a96b9ef9484cc6c1445255fc4f84206b48a65420efbedf16bdab55fbff`.
Download into a unique child of the existing job-owned `RUNNER_TEMP`;
verify the complete bounded archive before extraction. Use the existing
system `tar`, not another archive format or installation service.

The bootstrap checks both installed `cmake` and `ctest` identities before
appending their common absolute binary directory to `GITHUB_PATH`.
Publish the actual paths, version and archive digest in the setup log.
Retain the successful prefix for the job lifetime; preserve existing
temporary-root contents and never overwrite system tools. Failed download,
size/hash verification, extraction, missing executable or version agreement
must fail before PATH publication and remove only the newly owned prefix.
There is no alternate version, package
manager, image-tool or source-build fallback.

All four existing macOS installation bodies call that same bootstrap after
the architecture guard and before subsequent configuration. Existing Linux
and native Windows tool selection remains unchanged. This is an explicit
patched-tool profile, not a local replacement for CMake's GoogleTest module
or a new test runner.

Keep PRE_TEST registration, the default five-second enumeration limit,
every per-case deadline, case selection, assertion, allocation limit,
native workload, sanitizer model and fuzz control. Do not prewarm the test
binary, increase DISCOVERY_TIMEOUT, cache test results or treat discovery
as execution. Retain the two intentional PR nonexecuting contexts.

## Test-first, qualification and delivery

Add focused controls to the existing workflow-policy owner before the
bootstrap or workflow integration. Verify all four consumers use the one
installer and preserve their guards and workload commands. Exercise verified
publication and failures for invalid roots, incorrect/truncated archives,
extraction failure and mismatched tool versions without live network access.
Unsuccessful setup must not publish a success-shaped PATH entry.

Review the design, record a genuine missing-implementation/integration red,
then implement the smallest bootstrap. Run the focused and complete owning
tool selections and the actual source/batch plans. Reconcile unchanged
C++ sources, images, configurations and complete fast/runtime/native/fuzz
evidence rather than re-executing identical work or relabelling old results.
The new hosted tool profile still requires actual execution.

After independent acceptance, publish one normal fourth related source in
PR #294 and append its ledger record. Preserve CI41903485476 as failed;
do not rerun it. Require fresh exact-final-head standard CI and supplemental
quality/platform/fuzz bodies, followed by integrated review, normal squash
and actual-main qualification before expected-OID branch retirement.
General SQL prerequisites, IN and node43 remain incomplete.

## References

- [Official CMake discovery contract](https://cmake.org/cmake/help/v4.4/module/GoogleTest.html)
- [Affected PRE_TEST implementation](https://github.com/Kitware/CMake/blob/v4.4.2/Modules/GoogleTest.cmake)
- [Upstream discovery reversion](https://github.com/Kitware/CMake/commit/5e5467bfc3b5e5a4773a20a442bd36b6c5ff8480)
- [Official patched release](https://github.com/Kitware/CMake/releases/tag/v4.4.5)
- [Actual runner-image inventory](https://github.com/actions/runner-images/blob/macos-26/20260824.0517/images/macos/macos-26-Readme.md)
