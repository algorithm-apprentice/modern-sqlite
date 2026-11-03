# ADR-0202: Owned Fast-Runner Source Fixtures

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified main `d42f94ca`, tree `4738a270`, ledger207
- Related contracts: ADR-0102/0106 and ADR-0193

## Context and completed mechanism mapping

Qualification of the next native prerequisite exposed an existing fixture
ownership error. Complete GNU tool discovery ran80 tests and failed18
fast-runner cases at their original15-second parent deadline. Keep that
failed aggregate; the already passing differential population is separate.

`FastLocalTestCase` owns a temporary fake CTest build, but its CLI launch sites
execute the real checkout's `tools/run_fast_tests.py` with the checkout cwd.
The runner derives its source root from its own script path. Its before/after
repository identity therefore scans the whole developer checkout rather than
the source fixture whose protocol is under test.

A direct unchanged canonical `repository_identity` call on the current
mounted checkout measured37.052210601 seconds wall and2.3711875 seconds CPU.
One call exceeds the15-second fixture limit; ordinary execution calls it
twice. This observation is specific to this checkout and does not establish a
general filesystem-performance cause or permit a longer test deadline.

| Responsibility | Existing source |
|---|---|
| Temporary fake build and real-checkout CLI launch | `tests/tools/fast_tests_test.py:93-159` |
| Before/after source identity | `tools/run_fast_tests.py:390,417` |
| Complete tracked/untracked admitted source fingerprint | `tools/verify.py:129-144` |
| Canonical temporary Git repository initialization | `tests/tools/verification_test.py:20-28` |
| Native Windows tooling owner selection | `.github/workflows/ci.yml:194-197` |

The real runner imports `tools.verify`, whose policy comes from
`project/verification-policy.json`. `tools/__init__.py` must also be retained
so Python cannot select an unrelated package outside the fixture.

## Decision

Within the existing temporary-directory owner, create one small source
repository distinct from its fake build, modes and reports. Copy the exact
current bytes of `tools/__init__.py`, `tools/run_fast_tests.py`,
`tools/verify.py`, `project/verification-policy.json` and the root `.gitignore`.
Reuse `initialize_repository`; add no parallel initializer. The existing
ignore policy keeps generated Python bytecode outside the source fingerprint.

Route all three real CLI launch sites to the copied real runner and the owned
source cwd: ordinary execution, concurrent report writers and invalid
discovery. Direct in-process helper tests retain their normal production
imports. This temporary unit-test source is not a development worktree or a
clone of the engine.

Do not mock, cache, substitute or weaken repository identity. The real
before/after source, executable, environment, configuration and discovery
guards still execute. Do not add a production root override, modify the
production tools/policy, omit cases or change the15-second fixture and
30-second CTest deadlines. Retain protocol, XML, capture, timeout, report-lock,
process-group, cleanup and binary-drift assertions.

Add a distinct source-drift control: the fake GTest child changes an initially
present untracked canary in the owned source, and the otherwise successful
run must fail its real identity guard. The positive complete-union case also
compares the recorded source identity with the actual owned repository.
This proves that isolation does not discard source provenance.

## Verification and delivery

Retain the original failed suite and canonical fingerprint measurement.
Record the focused source-drift red before selecting the owned CLI fixture,
then require its expected identity rejection and the complete original
fast-runner/tool population under unchanged deadlines.

Reconcile platform domains explicitly. GNU runs the POSIX process-group
protocol fixtures. Native Windows retains its configured verification and
workflow-policy owners; it does not gain POSIX fast-local batching by
inference. Preserve current-source complete-fast evidence, graph/engineering
checks and independent implementation/evidence review before source208.

The uncommitted deadline-pair and C++ inspection candidates are preserved
separately. After this prerequisite's accepted normal commit and ledger,
restore/re-review the deadline-pair correction under ADR-0203, then the
inspection extraction under ADR-0204. Do not mix the independent changes into
one source commit, rewrite history or count paused/cancelled work as accepted.
Final exact-head hosted/main gates still precede delivery and branch cleanup.
General generation, IN query RHS, node43 and the project remain incomplete.
