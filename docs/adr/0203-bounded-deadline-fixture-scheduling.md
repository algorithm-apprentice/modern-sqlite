# ADR-0203: Bounded Deadline-Fixture Scheduling

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted ADR-0202 source208 `ac3341f2`, ledger208, draft PR #292
- Related contracts: ADR-0102/0106, ADR-0193/0197 and ADR-0201

## Context and completed mechanism mapping

Source208 isolates the fast-runner source fixtures without changing production
provenance guards. Resume the separately preserved deadline-pair correction;
its earlier code review was not source acceptance or delivery qualification.

The original native Python verifier owner exhausted its ten-second CTest
deadline during the last Windows fixture. A full-preset attempt took10.0345
seconds; exactly one reviewed isolated diagnostic took10.036 seconds. Both
entered38 methods and completed37. Preserve both failed attempts.

The harness import intervals were1.320393 and0.2343003 seconds. The repeated
failure does not justify attributing the problem to the first import interval
or the earlier hosted hashlib stall. Causes of wall-time variation remain
unproven.

One avoidable serialized cost is known. The existing absolute-deadline test
runs its noncancellable and cancellable-false variants sequentially, with one
second per child. They consumed2.0905375 and2.0836068 seconds in the retained
attempts. ADR-0201 already reserves two CTest processors for this owner.

| Responsibility | Existing source |
|---|---|
| Original immutable timeout fixture and both variants | `tests/differential/read_compatibility_tool_test.py:580-594` |
| Per-call protocol, limits and optional cancellation | `tests/differential/read_compatibility_tool_test.py:539-554` |
| Per-call process, temporary input, selector and absolute deadline | `tools/read_compatibility.py:1360-1451` |
| Deterministic kill/reap and pipe cleanup | The same `_run_bounded` finally block |
| Existing exclusive two-slot budget | `tests/CMakeLists.txt:2306-2314` |

The calls read one immutable runner/database/protocol. Each owns its input
stream, process, selector, capture buffers and deadline. Neither uses a shared
mutable mock or cancellation event, and no other test method executes
concurrently with them.

## Decision

Schedule only these two existing captures through the standard
`ThreadPoolExecutor`, with exactly two workers. Submit both before collecting
either result. Retain the original `None` and false cancellation predicates,
one-second child deadlines and fixture contents.

Keep the original `subTest` and timeout-message assertions on the main unittest
thread. Collect the futures in the original variant order. Executor context
exit waits for both tasks, including failure paths, before the testcase's
ordinary fixture cleanup. Do not execute unrelated methods concurrently or
add another production scheduler.

Keep the ten-second owner deadline, two-slot reservation, command, interpreter,
all38 native methods, original assertions and CLI invocations, capture limits,
import/extension/phase diagnostics and security settings unchanged. Production
verification and oracle provenance are unchanged. Removing this serialization
is not a timing guarantee or a general host-stall fix.

## Evidence boundaries and resumed qualification

The preserved implementation observed exactly two simultaneously live child
PIDs, both original timeout errors, and complete process/pipe/input/fixture
cleanup. A changed-source verbose collection still failed10.0422 seconds.
One reviewed canonical `--output-on-failure` collection then passed7.8653
seconds with all38 methods and complete diagnostics retained in `LastTest.log`.
Do not claim that output display caused the failures or that the failed
aggregates passed. Those observations belong to the preserved candidate,
not automatically to this resumed source.

Reconcile source, inputs, runtime images, environment and discovery before
reusing any matching observation; otherwise execute the original gate on the
resumed source. Retain complete raw diagnostics without needing live verbose
output. Keep all earlier failures and do not repeat failed unchanged-source
attempts automatically.

Both original focused assertions already passed; this test-only scheduling
change does not invent a production TDD red. Require real bounded overlap,
original errors and cleanup, the complete native/GNU owners under unchanged
deadlines, source208's intact tool-fixture protection, and complete-fast
evidence. Review this renumbered design before restoring the owned method.

Independent source/evidence acceptance precedes a normal source209 commit and
append-only ledger. Only then resume the separately preserved inspection
extraction under ADR-0204. Final exact-batch-head CI and integrated review
must pass before squash merge. Qualify the resulting actual-main commit
after merge and before branch cleanup and delivery closure.
General generation, IN query RHS, node43 and the project remain incomplete.
