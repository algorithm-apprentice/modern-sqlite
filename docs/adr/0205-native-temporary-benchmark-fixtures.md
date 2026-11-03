# ADR-0205: Native Temporary Ownership for Benchmark Contract Fixtures

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source210 `95dbde5d`, ledger210, draft PR #292
- Related contracts: ADR-0102/0106 and ADR-0204

## Evidence and scope

Source210's original read-benchmark CLI gate exceeded its unchanged300-second
deadline. Setup-only and one bounded phase observation did not reproduce a
setup failure: the phase observation spent20.175 seconds in Modern's fit
point-present diagnostic child and17.154 seconds in SQLite's equivalent child.
The original failure remains a failure; the observations do not uniquely
attribute its cause.

One separately admitted four-cell profile comparison used the same binary,
fixture bytes,8 selected iterations,4096 warmup iterations, and verification
work. For each engine, the repository-v9fs path and an ext-family Linux-local
copy produced identical complete work and lifecycle reports. Both engines'
observed wall and CPU costs differed materially by location. This supports a
fixture-location ownership correction, not an engine optimization or a
general speed claim.

## Existing-owner inventory

- `read_benchmark_cli_test.py`: two manifest-authenticated fixtures retained
  directly as repository paths by `setUpClass`.
- `index_benchmark_cli_test.py`: one authenticated indexed fixture retained
  directly; child benchmark write modes already own their mutable scratch.
- `order_by_benchmark_cli_test.py`: one authenticated ordered fixture retained
  directly; existing standard-library allocation limits remain unchanged.
- `in_benchmark_cli_test.py`: one authenticated fixture retained directly;
  existing smoke/baseline/diagnostic selections remain unchanged.
- `distinct_compound_benchmark_cli_test.py`: one authenticated fixture retained
  directly, with existing malformed-report controls.
- `aggregate_benchmark_cli_test.py`: one authenticated fixture retained
  directly, with existing configure/restore lifecycle.
- `preparation_benchmark_test.py`: one supplied database path, with existing
  pre/post byte hashing and child lifecycle/fault assertions.

Write benchmark owners already use invocation-owned temporary scratch;
fixture regeneration already writes to temporary directories. Neither is
changed. C++ fuzz fixture mechanisms, production engine code, benchmark
algorithms and ordinary baseline-run database selection are outside scope.
`read_performance._run_one_baseline_child` already implements the canonical
copy-and-SHA256 check for archived baseline scratch at3104-3110. Extract that
exact primitive once and reuse it; keep its existing error text, return digest,
output path and preserved failure artifacts unchanged. The new class-lifetime
context must not duplicate that copy/authentication algorithm.
Reuse `read_performance._sha256` and `_sidecars`, `tempfile.TemporaryDirectory`
and `contextlib.ExitStack`. `README.md:64` retains Python3.10 support; do not
require `unittest.enterClassContext`. Index's `StatefulDatabaseCopy` already
owns mutable sibling copies at `index_performance.cpp:2061-2110`; staging its
immutable input also places those existing siblings inside invocation-owned
storage without changing that algorithm.

## Shared mechanism

Add one context-managed fixture-copy helper alongside the existing shared
read-performance input helpers, not one implementation per owner. Accept an
identifier-to-(source-path, expected-SHA256) mapping and yield the corresponding
owned database paths. Use the host's normal temporary-directory mechanism,
not a checkout/build-relative directory or hardcoded filesystem override.
Keep source basenames in separate ordinal subdirectories to avoid collisions.

Validate source bytes against the supplied expected digest and reject source
journal/WAL/SHM sidecars rather than silently dropping their state. Reuse the
extracted canonical primitive to copy/authenticate complete bytes before
yielding. Manifest owners supply their existing pinned fixture digests after
the unchanged original manifest validation. Preparation supplies its existing
pre-run source digest.
Keep original repository/profile/workload paths and validation unchanged;
only database paths passed to the existing child commands become owned copies.

The context owns cleanup on partial setup, normal return and exceptions.
Detect unexpected source/copy mutation or sidecars rather than silently
refreshing or deleting the evidence of a failed readonly contract.
Always remove only its own temporary directory; never alter or remove the
repository fixtures. Surface I/O and authentication failures explicitly with
the existing `HarnessError` contract, retaining exception context.

Each existing unittest class uses a Python3.10-compatible `ExitStack` registered
with `addClassCleanup` before acquiring the copy context. This preserves cleanup
if later class setup fails. Reuse one copied fixture across that owner's
unchanged child matrix; do not change SQL, repetitions, cache size, journal,
synchronous or mmap settings, allocation limits, timeouts, output ceilings,
providers, assertions, or declared test populations.

## TDD and verification

Use the existing `read_performance_tool_test.py` owner. Add focused deterministic
tests that fail before the helper exists and prove independent authenticated
copies, cleanup after partial-copy failure, and mutation/authentication/sidecar
rejection without damage to source bytes. These tests exercise the helper's
ownership contract, not filesystem speed or standard-library behavior alone.

Run focused red/green and the complete existing owning tool suite. Reconcile
unchanged C++ fast evidence without relabeling earlier execution. Re-run the
actual failing canonical read CLI owner under its unchanged300-second bound
only after the reviewed source change; then complete all affected native
owners and the required final exact-head full-plan/hosted/integrated gates.
Keep the prior failed gate and all characterization results in the ledger.

## Delivery

This is the fourth related source in draft PR #292, after accepted source210's
normal commit and ledger. It cannot make the batch, node43, general generation
or IN-query work complete by itself.
