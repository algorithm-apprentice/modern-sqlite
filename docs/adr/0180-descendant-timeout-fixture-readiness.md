# ADR-0180: Descendant Timeout Fixture Readiness

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0102, ADR-0106, ADR-0179

## Completed inventory and bounded finding

The two failure-atomic savepoint implementation slices in draft #275 are
accepted and locally qualified as `98c1014` and `1527cda`, sealed in ledger
115/116. This does not satisfy their final hosted qualification.

The informative first-prefix CI `41080041817` passes Native Windows and seven
other contexts, but macOS Debug fails
`performance.read_performance_tool`. Its descendant timeout case reads a
missing `descendant.pid` after the original 0.2-second execution deadline.
The performance test and production runner are unchanged from qualified base
`26a0002`. This is not a savepoint or Windows-engine failure.

The parent directly inventories the complete relevant owner chain:

- The test launches a Python parent. That parent launches a real descendant,
  writes its PID and exits. The descendant inherits the captured pipes.
- `run_bounded` owns one real `Popen`, a new process group, both pipes and its
  selector. It computes its execution deadline after process construction and
  selector registration.
- At timeout, the existing `_stop_process_group` signals the owned group,
  waits within its existing bound, escalates if necessary and reaps its
  leader. The test independently checks that the descendant disappears.
- No shared readiness fixture exists. The test currently assumes its parent
  has published the descendant PID before the execution deadline expires.

The unchanged focused case passes locally. A real controlled 0.3-second
parent startup delay, with the original 0.2-second timeout and 32-byte capture
limits unchanged, deterministically reproduces the missing-PID failure.
This proves the test's missing readiness precondition, not the exact
historical macOS scheduling event or syscall; that run did not retain the
child's captured stderr.

## Decision

Correct only this owning Python test fixture. Do not change `run_bounded`,
its constructor contract, deadline origin, process-group cleanup, public
tool API, benchmark workloads or engine code.

Extract the original descendant assertion into one private test helper used
by the existing case and the controlled-startup regression. A test-local
constructor adapter delegates to the original real `subprocess.Popen` with
the unchanged command, directory, streams and new-session arguments. It
returns that same real process only after its descendant fixture is ready.
It does not replace a clock, process, PID, selector or termination operation.

The parent publishes its one descendant PID through an atomic sibling-file
rename after writing complete ASCII contents. Bound readiness by two seconds
after real process construction returns, independently of the unchanged
0.2-second descendant execution deadline. Verify a positive, live descendant
PID in the actual parent's process group. Only then return the real process to `run_bounded`;
its existing deadline and actual cleanup remain authoritative.

This separates fixture construction from the descendant cleanup assertion.
The original unadapted 0.1-second cold-start timeout, two-second oversized
output case, two-second ordinary completion and all capture/exit assertions
remain unchanged. Slow or failed readiness is an explicit test failure,
never a skipped descendant check or a successful timeout result.

If construction succeeds but readiness fails, retain fixture ownership:
reuse the existing canonical group cleanup, close both pipes and propagate
the failure. Do not invent another termination algorithm or leave a child
behind. Successful readiness transfers the same process and streams to the
unchanged runner.
Do not reap the parent before failed-setup group cleanup; retain its identity
until the canonical owner retires it.

## Test-first and delivery gates

First extract the original assertion from its green focused baseline and add
the controlled 0.3-second startup case. Before implementing readiness it must
fail with the observed missing descendant PID under the unchanged 0.2-second
execution limit. Then add the smallest real readiness adapter.

Cover ordinary and delayed startup, parent exit before publication and a live
parent that never publishes readiness. Failed setup must retain explicit
diagnostics and clean its owned process resources. Keep the existing actual
descendant termination assertion and every original method; add coverage
instead of weakening them.

Require the focused red/green, complete performance-tool methods, complete
fast units, graph/engineering checks and implementation review. Reconcile
unchanged compiler/sanitizer inputs rather than claim this Python change
revalidates another configuration. Final exact-head nine-context CI, the one
actual full-quality dispatch and integrated review remain required.

This is one localized final-gate remediation commit under ADR-0106, after
the two already accepted storage/transaction commits. It adds no production
slice, native mechanism, SQL feature or later DAG node. Data-statement epoch,
schema cleanup/retry, no-op classification and preparation ownership remain
separate. No blind workflow rerun, deadline increase, capture-limit change,
assertion removal or policy bypass is admitted.
