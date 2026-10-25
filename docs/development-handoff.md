# Cross-Machine Development Handoff

Traceability: `implement-advanced-sql`, ADR-0053, ADR-0102, ADR-0106, ADR-0111.

This document, the repository and the linked pull-request evidence are the
portable resume state. A previous machine's Copilot session directory,
absolute paths, build trees and binaries are not prerequisites.

## Resume the project

```sh
gh repo clone algorithm-apprentice/modern-sqlite
cd modern-sqlite
git switch main
git pull --ff-only
git status --short
python3 tools/project_graph.py validate
python3 tools/project_graph.py next-ready --json
```

Read `AGENTS.md`, `project/manifest.json`, `docs/architecture.md`,
`docs/dependency-dag.md`, the applicable ADRs, the selected module-graph node
and `project/progress.json` before production changes.

The accepted handoff baseline is squash commit
`eee2865957907b7d177a7fe91d3982f48a9df1d2`, tree
`672a99c599d8687ae133351efa6e42892ce12ddc`, merged in
[PR #257](https://github.com/algorithm-apprentice/modern-sqlite/pull/257).
Later accepted work on `main` supersedes this baseline; do not reset it.

Node 43, `implement-advanced-sql`, is still in progress. Nodes 1-42 are
complete; nodes 44-48 are not. The next-ready command must still select the
active node until its completion is explicitly recorded.

[PR #258](https://github.com/algorithm-apprentice/modern-sqlite/pull/258)
contains the subsequent scalar-list IN delivery. Check its state before
selecting new work. If still open, resume its latest published head with
`gh pr checkout 258` in a clean worktree and finish its outstanding gates;
do not restart its implementation from the older merged baseline.
If merged, use current `main` and continue in the order below.

## Recreate the reference and builds

Install CMake 3.25+, Ninja, Python 3.10+, a C++23 compiler, a C compiler,
Make and Tcl. See the committed CI workflow for the currently qualified
compiler and quality-tool configurations.

An ordinary development build needs no local SQLite installation:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel 2
python3 tools/run_fast_tests.py --build-directory build/dev-debug
```

For source research and native contracts, recreate the pinned reference in a
sibling directory, not inside the production source tree:

```sh
git clone --no-checkout https://github.com/sqlite/sqlite.git ../sqlite-source
git -C ../sqlite-source checkout --detach \
  cb547ab3e931c7766e24834af5ef6c4578863e3d
test "$(cat ../sqlite-source/VERSION)" = "3.54.0"
test "$(cat ../sqlite-source/manifest.uuid)" = \
  "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
mkdir ../sqlite-build
(
  cd ../sqlite-build
  ../sqlite-source/configure
  make sqlite3.c -j2
  printf '%s  %s\n' \
    '4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6' \
    sqlite3.c | shasum -a 256 -c -
  printf '%s  %s\n' \
    'ab12aaa090d3921ba8fd2c8c198ae1e8cafce79fd9897a5b89830fab1f6ecef5' \
    sqlite3.h | shasum -a 256 -c -
)
cmake --preset benchmark \
  -DMODERN_SQLITE_PINNED_SQLITE_C="$PWD/../sqlite-build/sqlite3.c" \
  -DMODERN_SQLITE_PINNED_SQLITE_H="$PWD/../sqlite-build/sqlite3.h"
python3 tools/verify.py build-native \
  --build-directory build/benchmark --parallel 2
ctest --test-dir build/benchmark -L 'performance|compatibility' \
  --output-on-failure --no-tests=error
```

Use this local pinned checkout for subsequent source research. Do not
substitute a system SQLite, a current upstream checkout or old-machine
libraries. Read the existing benchmark drivers' `--help` for fresh matched
measurements; keep timing isolated from builds, fuzzing and other timing runs.
Never copy CMake caches or compiled libraries between machines.

## Accepted evidence and implementation

The following evidence is accessible without the original machine:

- [Complete PR #250 batch ledger](https://github.com/algorithm-apprentice/modern-sqlite/pull/250#issuecomment-6383338442).
- [Accepted final integrated review](https://github.com/algorithm-apprentice/modern-sqlite/pull/250#issuecomment-6383449210).
- [Exact-head CI](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40085673270)
  and [Full quality](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40085673211).
- [PR #251 complete ledger](https://github.com/algorithm-apprentice/modern-sqlite/pull/251#issuecomment-6384868212),
  [final acceptance](https://github.com/algorithm-apprentice/modern-sqlite/pull/251#issuecomment-6384933361),
  [CI](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40093076681)
  and [actual Full quality](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40093280711).
- [Final VM/shared-read attribution](https://github.com/algorithm-apprentice/modern-sqlite/pull/251#issuecomment-6385037157).
- [PR #253 complete ledger](https://github.com/algorithm-apprentice/modern-sqlite/pull/253#issuecomment-6385832922),
  [final acceptance](https://github.com/algorithm-apprentice/modern-sqlite/pull/253#issuecomment-6385886816),
  [CI](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40103621916)
  and [actual Full quality](https://github.com/algorithm-apprentice/modern-sqlite/actions/runs/40103723473).
- [PR #255 source qualification](https://github.com/algorithm-apprentice/modern-sqlite/pull/255#issuecomment-6387563927)
  and the [committed parser/preparation archive](../benchmarks/preparation-baseline-v1.json).
- [PR #256 read-preparation repair](https://github.com/algorithm-apprentice/modern-sqlite/pull/256),
  [ADR-0150](adr/0150-native-cached-schema-read-preparation.md) and the
  [cached-schema phase archive](../benchmarks/cached-schema-preparation-v1.json).
- [PR #257 writable preparation and audit closure](https://github.com/algorithm-apprentice/modern-sqlite/pull/257),
  [ADR-0151](adr/0151-native-cached-schema-write-preparation.md) and the
  [portable frontend closure archive](../benchmarks/preparation-closure-v1.json).
- [PR #258 scalar expression-list IN](https://github.com/algorithm-apprentice/modern-sqlite/pull/258),
  [ADR-0152](adr/0152-native-scalar-in-expression-lists.md) and the
  [portable scalar-IN archive](../benchmarks/in-baseline-v1/aggregate.json).
- Committed ADRs 0123-0152 and owning-module tests, which preserve the source
  maps, contracts and executable regressions.

PR #250's final tree passed complete fast units; six isolated compiler/runtime
configurations; 58 native contracts; 60 fresh matched cases across five
families; four 20,000-run generated-input campaigns; and quality gates.
The public ledger records exact identities, selections and evidence digests.
Hosted PR contexts that intentionally skip workloads are not executed-test
evidence. Raw old-machine diagnostics are supplementary, not a portable
evidence cache or a reason to reuse differently configured results.

PR #250 replaces eager compound materialization with native producer/merge
scheduling through the existing compiler, coroutine VM and comparator.
Preserve one query owner and lexical register layout, local versus inherited
collations, native lookahead/EOF scheduling, and duplicate/OFFSET/output/LIMIT
order. Group-order proofs retain syntactic ordinal/COLLATE distinctions.
Concurrent arm sorters retain the shared statement budget and two-file bound.
No IN, derived FROM, CTE, view or window feature was added.

PR #251 adds native two-pass stored-index ordering for nondefault NULL
placement through the same producers. It preserves the equality key across
both partitions and skips later erroring rows when native LIMIT does.
Its source has 1,766 ordinary plus 97 standalone fast cases, all six isolated
configurations, 58 native contracts, 60 fresh matched cases, four original
20,000-run campaigns and accepted exact-tree review. The disclosed
sleep-invalidated local attempt was requalified without changing source or
deadlines. Task branches were cleaned up; the accepted code is on `main`.

PR #253 replaces cloned numeric function arguments with canonical borrowed
observations, without changing public owned outputs, R1 typing or state
algorithms. Numeric cold/warm allocation is now exactly one/zero, within the
original cap; independent exact controls remain. The original
`global-functions-memory` Modern median changes from 143.4 ms to 136.1 ms, with matched ratio
5.351735x to 4.975597x, not a whole-engine parity claim. The current source
passed the actual targeted plan, complete fast 1,768+97, native 58, all 60
fresh matched guards and exact-head hosted checks. Unselected full/fuzz runs
are not claimed. Entry 13's supported function disposition is closed.

PR #255 adds reproducible phase-separated SELECT frontend evidence. Its
qualified source is `62848b7`, tree `3c81faba4e2a7905a223a30068400ec946d76ab0`.
The archive contains all raw reports and nine samples per engine/phase;
`tools/preparation_performance.py validate-historical-baseline --baseline
benchmarks/preparation-baseline-v1.json` validates it without old-machine
builds. Entry 14 retains the source-preserving parser as a scoped measured
alternative. Pure syntax ranges from 1.071243x to 5.759859x native in this set;
complete short preparation at that baseline is 15.643751x. No whole-engine
parity or new SQL surface is claimed. Entry 15's later closure is recorded
under ADR-0151 below.

PR #256's qualified source `3658b0e`, tree
`47639f6a80d135d6245efa1d2643f1c7004a67de`, translates native cached-schema
admission through the read facade's existing catalog, cleanup and compiler.
Successful warm preparation makes zero VFS calls instead of seven. Short
prepare is 3.763 us Modern versus 0.472 us native, ratio 7.966699x, in the
fresh matched archive; the historical Modern median is 7.293 us. All sixteen
phase/shape pairs, exact work and separate allocation/lifecycle controls are
retained. Validate the new archive with the same historical command and its
repository-relative path. All six local isolated configurations, original
models, native contracts, four bounded campaigns and production quality pass.
That read-only correction did not complete the write facade or entry 15.

PR #257 qualifies source `e65afd2`, tree
`848facca5b5d815dac228a1dfbb72ce13fe76a81`, for the shared cached-schema
preparation and writable publication/rollback correction. Preparation uses
one private read owner and one retry path, never VM-statement rollback policy.
Writable warm VFS work changes 56 calls over eight operations to zero; the
exact no-op allocation contract tightens 22 to 21. Native-equivalent DML/DDL
provenance, same-cookie rollback invalidation and retained cleanup remain
explicit. No bytecode isolation/verifier or query-owner contract is bypassed.

The actual source plan passes six isolated configurations (1,930 each,
1,951 instrumented), complete fast 1,791+97, native 59, four original bounded
campaigns and production quality. Fresh existing write-version-2 evidence
keeps both profiles and all nine workloads; matched-durable guards pass
unchanged, while engine-default stays informational. The new portable archive
retains all sixteen read parse/prepare pairs, not writable-prepare timing;
short read prepare remains 8.234204x native. Validate it with the same
historical command and its repository-relative path.

ADR-0151 closes the binder/preparation inventory as an admitted architectural
translation. The roll-up is 96 paths / 36 mechanisms: 14 reference-faithful,
20 architectural translations and two measured alternatives, with no current
unresolved or repair-required row. PR #257 completed final exact-head review,
hosted acceptance and merge. Do not restart the closed function/parser/VM work
or treat those measured costs as an unbounded optimization mandate.

PR #258's clean measured source is `86c026e`, tree
`bc094b7d9af8e6588039a751e8dc4afda50dde46`. It adds empty, singleton,
linear and reusable-set scalar expression-list `IN`/`NOT IN` through the
existing compiler, VM and temporary B-tree. Frozen membership survives
private/coroutine boundaries but not whole-execution cleanup. A subsequent
constructor-flag repair preserves the native 32-bit INTEGER/QNUMBER boundary
before AND deletion and list classification. The current owning native shard
contains 83 comparisons and two deferred query-RHS controls; its earlier
76 records, the original 512-case corpus and older native records are unchanged.

All five new matched cases have nine samples per engine, exact typed work,
separate allocation diagnostics and unchanged guards. Their aggregate CPU
ratios range from 3.299240x to 4.591668x native; this is not speed parity.
Validate `benchmarks/in-baseline-v1` with
`tools/in_performance.py validate-historical-baseline` and the committed
`tests/performance/in-workloads-v1.json`. The archive retains its measured
source prefix, not an assertion that later final-head gates already ran.
Complete the PR's actual integrated review, compiler/runtime, native,
generated-input and hosted gates before consuming the delivery.

## Continue in the existing order

Use [the reference audit](sqlite-reference-audit.md) as the mechanism ledger
and [ADR-0053](adr/0053-sqlite-aligned-advanced-sql-architecture.md) as the
feature order, not the old session's todo counts:

1. Audit entry 12's confirmed shared-read/VM corrections are closed by the
   completed native range, compound and two-pass NULL-order delivery.
   Do not reimplement those mechanisms from an older session's pending notes.
2. Function ownership/state resolution (13), the scoped measured parser
   alternative (14), and binder/preparation (15) have supported dispositions.
   PR #257's merged closure is the accepted prerequisite.
   Do not turn classification into an open-ended optimization campaign.
3. Finish PR #258 if still open. Then continue IN query RHS in ADR-0053 order,
   mapping native existing-index/rowid, materialization and Bloom mechanisms
   before design. SELECT/VALUES RHS, promoted singleton SELECT, row values,
   named shorthand and new IN-driven WHERE loops remain deferred.
   Derived FROM/CTEs/views remain later work; scalar-list IN does not
   complete node 43.
4. Finish node 43 before concurrency/locking, WAL, C API, extensions and
   hardening (44-48).

Continue one coherent reviewed slice at a time, with main-agent design and
implementation, English artifacts, strict C++23, test-first behavior changes
and the canonical ownership/error mechanisms. Review agents use GPT-6.1 Sol.
Use `tools/verify.py plan` for the actual diff and
[the verification policy](verification.md); do not repeat successful,
unchanged qualification just to move machines.

Unpushed work and stashes are machine-local and are not part of this clean
baseline. If moving during a later slice, first review and publish its
buildable commits and append-only evidence to its draft PR, and record its
exact head and remaining gates there. Do not upload an entire Copilot state
directory or assume that remote session access transfers local files.
