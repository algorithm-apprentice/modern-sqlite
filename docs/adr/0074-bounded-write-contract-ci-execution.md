# ADR-0074: Bounded Write Contract CI Execution

- Status: Accepted
- Date: 2026-10-13

## Context

The post-merge CI run for source `b9c4c70` timed out
`performance.write_benchmark_cli` after 180 seconds on Ubuntu. Every other
benchmark and fixture contract passed. The identical reviewed tree's PR job
completed the write contract in 74.93 seconds. An unchanged replay of the
failed main job completed in 62.40 seconds; the original failure remains
preserved and is not replaced by that replay.

The write CLI contract contains 36 timing-smoke children and 36 full-work
diagnostic children, plus protocol/profile checks. The frozen runner bounds
each timing-smoke child at 120 seconds and each diagnostic child at 600 seconds.
CTest instead applies one 180-second bound to the whole combined suite.
The failed log reports neither the active phase nor its engine/profile/case.

This does not establish an engine deadlock or deterministic performance
regression. It establishes a watchdog and diagnostic-granularity defect:
a parent can terminate compliant children cumulatively before their own
explicit bound or report can complete.

## Decision

Keep timing-smoke and protocol coverage in the existing
`performance.write_benchmark_cli` test with its existing 180-second bound.
Move the full diagnostic matrix into independent CTest cases, one for every
case/profile/engine combination.

Each diagnostic case invokes the same canonical test validation and frozen
runner for exactly one child. The runner's 600-second limit remains unchanged;
the CTest parent has 630 seconds, providing 30 seconds for interpreter setup,
validation, cleanup, and reporting. The workflow's existing overall deadline
remains a separate hard bound.

The existing script's default remains the complete suite for manual use.
Explicit smoke or diagnostic selection is test orchestration only, not a new
benchmark mode. Reject incomplete or unknown diagnostic selections. Do not
change SQL, corpus, repetitions, diagnostic work, durability, counters, result
digests, or performance guards.

Print flushed start and completion records with phase, engine, profile, and
case around each validated child. These are parent test diagnostics, not
instrumentation in the ordinary engine or measurements used as speed claims.
Future stalls identify the exact running child, and its bounded runner still
propagates timeout/error evidence rather than returning successful EOF.

The existing benchmark-contract CI selector must execute all newly registered
diagnostic cases. Preserve identity, fresh-image, strict-result, invalid-input,
profile-replay, immutable-fixture, and final-content checks.

## Verification

- Focused red tests demonstrate the missing per-child registration/CI selector
  and explicit-selection/progress behavior before implementation.
- The CTest matrix contains exactly 9 cases x 2 profiles x 2 engines.
- Every diagnostic combination validates the same fixed-work report.
- The default complete suite and explicit smoke/diagnostic modes remain valid.
- Invalid or incomplete selections fail explicitly.
- Existing benchmark source, runner, workload, fixture, and archived baseline
  input hashes remain unchanged.
- Record the original timeout, identical passing PR work, and unchanged replay
  separately; never describe a replay as erasing the failure.

## Traceability

Node: `implement-advanced-sql`, PR #184 post-merge gate closure.

Contracts: ADR-0050, ADR-0046, and the existing bounded benchmark runner.
