# ADR-0168: Write-Model Watchdog Progress

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Related decisions: ADR-0106, ADR-0153, ADR-0166, ADR-0167
- Predecessor: reviewed PR #269 head `67115a6`, tree `e7798f7c`
- Design accepted before implementation at proposal `1499dae6`.

## Measured final-head outcome

The three accepted slices pass their complete native and sanitizer fast
selections. Intermediate head `7dc5664` passes all nine actual CI contexts.
At final head `67115a6`, CI run 40866826889 passes eight contexts, including
macOS Debug and sanitizer, but native Windows stops at
`WriteSessionModel.MatchesDeterministicMixedActions`. The original 20-second
watchdog reports 20.06 seconds elapsed. The captured output contains no action
or completed model phase.

The same model passes the restored canonical local native Debug configuration
in 1.11 seconds. This is not a reproduction or a passing hosted gate.
The blocked operation and cause remain unknown.

## Decision

Add exactly one fourth coherent test-only qualification commit to PR #269,
within ADR-0106's normal four-commit bound. Reuse the canonical `PhaseTiming`
and the model's existing action identities instead of another clock, model,
generator or executor.

Flush progress around temporary-directory setup, initial open, action
generation, each original action execution and state comparison, final
rollback and scope exit. Record the original seed, generator, action index
and kind with elapsed and preceding-phase time. Scope exit is not success.
Preserve the current action history for actual assertion failures.

Keep all 256 actions, generated words, prelude, transaction/savepoint behavior,
reopens, full comparisons, row/savepoint bounds, action-kind coverage and the
whole 20-second deadline. Do not select weaker synchronization, batch fewer
actions, replace native files, change worker ownership or add a retry.
No production library or SQL feature changes.

This improves the evidence retained by a watchdog; it is not an asserted
storage or hosted-environment diagnosis. A subsequent timeout must be
investigated from its entered/completed action and phase.
No fifth commit or additional feature is authorized by this decision.

## Qualification

Record a focused missing-progress observation against the original model and
verify that the actual changed model retains its successful comparisons and
all 256 action identities under the original bound. Run complete fast units,
the affected model and the actual change-aware closing gates. Keep failed
hosted heads explicit until final exact-head selections genuinely pass.

Review design and source, seal append-only ledger evidence, then qualify the
final four-commit head before guarded squash. Verify actual resulting main
and clean only the delivered local and remote branch. Node 43 and the project
remain incomplete.
