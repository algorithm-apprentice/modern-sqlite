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
`64f93e4e0d82f6665e08c8eae58746a0439d0e20`, tree
`a61e92e6e97cf86417ee1a3ad7fab53a816b8efc`, merged in
[PR #251](https://github.com/algorithm-apprentice/modern-sqlite/pull/251).
Later accepted work on `main` supersedes this baseline; do not reset it.

Node 43, `implement-advanced-sql`, is still in progress. Nodes 1-42 are
complete; nodes 44-48 are not. The next-ready command must still select the
active node until its completion is explicitly recorded.

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
- Committed ADRs 0123-0147 and owning-module tests, which preserve the source
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

## Continue in the existing order

Use [the reference audit](sqlite-reference-audit.md) as the mechanism ledger
and [ADR-0053](adr/0053-sqlite-aligned-advanced-sql-architecture.md) as the
feature order, not the old session's todo counts:

1. Audit entry 12's confirmed shared-read/VM corrections are closed by the
   completed native range, compound and two-pass NULL-order delivery.
   Do not reimplement those mechanisms from an older session's pending notes.
2. Resolve functions (entry 13), then parser (14), then binder (15), with
   supported dispositions or bounded repairs. Do not turn classification into
   an open-ended optimization campaign.
3. Resume remaining node-43 SQL in ADR-0053 order only after the implemented
   inventory has no unresolved or repair-required entry.
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
