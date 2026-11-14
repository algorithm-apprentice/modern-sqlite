# Current Development Handoff

Updated: 2026-11-14. Traceability: `implement-advanced-sql`, ADR-0106,
ADR-0111, ADR-0255, ADR-0256, ADR-0257, ADR-0258, ADR-0259, ADR-0260.

This is the current portable starting point. The repository and linked
pull-request evidence are authoritative; an earlier machine's session
directory, binaries and absolute paths are not prerequisites.

## Qualified main and retained history

[PR #331](https://github.com/algorithm-apprentice/modern-sqlite/pull/331)
qualified main `9be3099329579c7ca67cfa4295adea87f1406207`, tree
`eb42487daaa664d53c8d23de1c27492133ea0561`, delivery ledger394.
Its [qualified handoff](https://github.com/algorithm-apprentice/modern-sqlite/pull/331#issuecomment-6843224205)
records source acceptance, original actual-main execution and expected-OID
source retirement. Its later automatic Full quality also succeeded.

[PR #332](https://github.com/algorithm-apprentice/modern-sqlite/pull/332)
merged the accepted SELECT entry correction as
`3166e15e6cc0c2511350f5c08042c005f8a370e3`, tree
`14a5bb1cabffd76eedc8424ade225f0eb072c58e`. Its local and original PR gates
passed, but [original actual-main qualification was blocked](https://github.com/algorithm-apprentice/modern-sqlite/pull/332#issuecomment-6844687498),
ledger396. Original CI43366640560 timed out the Windows policy owner and Mac
Release dependency-analysis owner under their unchanged ten-second limits.
Automatic Full quality43366640341 separately exceeded45 minutes; its log
archive is empty, so no failed translation unit or unique cause is established.

[PR #333's qualified descendant handoff](https://github.com/algorithm-apprentice/modern-sqlite/pull/333#issuecomment-6857728951)
closes that delivery block at main
`71c77ed0587a98a25256f914550456aee380840c`, tree
`b588785189b37a2e5a4cacab8649f4b666b2d207`, ledger402. The original main CI
passed all ten roles, and the separately reviewed source Full quality
execution passed under the unchanged budget. Its disclosed one-shot
diagnostic allowance is closed.

The approved source branches were retired with expected-OID guards; all four
stashes remain preserved. Retain the original failures and unexecuted
suffixes as failed evidence: later qualification does not establish their
cause. Do not reset main, reopen completed delivery controllers or reuse an
earlier diagnostic request allowance.

The active DAG node is still `implement-advanced-sql`; nodes 44-48 remain
blocked. Use [project progress](../project/progress.json) and the
[module graph](../project/module-graph.json) for node state, not historical
test totals or a previous session's todo count.

## Current construction prerequisite and next SQL boundary

[ADR-0256](adr/0256-executable-change-aware-verification.md) owns canonical
plan execution and truthful analysis receipts. PR #331 separates declared
report outputs from immutable inputs; PR #332's fresh fifteen-step local
plan subsequently passed without relabelling the earlier failed plans.

[ADR-0258](adr/0258-canonical-analysis-tool-identities.md) selects one measured
tooling correction: use the existing command-file identity helper once per
analysis snapshot instead of reading one resolved executable separately for
aliased roles. Keep all before/after checks and fail incomplete tool snapshots
before launch. This removes duplicate work, not the historical failure record
or any required gate. PR #333 completed descendant qualification and the
approved source retirement described above.

[ADR-0259](adr/0259-canonical-ordinary-construction-events.md) selects the next
bounded prerequisite: share the existing zero-argument wildcard classification,
then use the existing construction-event owner for ordinary function and
parameter ordering. Its two increments are accepted in
[PR #334](https://github.com/algorithm-apprentice/modern-sqlite/pull/334);
delivery remains pending. Original Windows startup timeout and full-quality
cancellation records remain unqualified, and the direct replay proposal was
rejected.

[ADR-0260](adr/0260-native-python-startup-budget.md) supplies a normal
configuration correction: this one Windows Python owner gets thirty seconds
for startup, retaining ten seconds after full module readiness. It does not
relax functional/child limits or repair the separate quality cancellation.
Qualify the actual corrective head before merging or retiring its source.
Private source
preparation and `NC_NoSelect` remain the larger subsequent work. The
established source prerequisites are:

- [ADR-0254](adr/0254-canonical-source-binding-scope.md): one active/saved
  source-state owner; delivered by PR #328.
- [ADR-0255](adr/0255-canonical-source-namespace-operations.md): canonical
  wildcard and USING enumeration with immediate borrowed column views;
  delivered by PR #329 without changing preparation-phase behavior.

[ADR-0257](adr/0257-canonical-select-initialization-entry.md) supplies the
bounded entry correction in current main: common SELECT initialization
precedes existing query-width admission. It does not close source expansion,
general constructor ordering or `NC_NoSelect`. Its earlier main delivery
block is now closed by the qualified descendant, not retroactively passed.

The next design must distinguish source expansion, name resolution and
later value generation. Pending wildcard/USING diagnostics can be
superseded, but later preparation-entry checks can stop traversal. Compound
core and LIMIT ordering, constructor-owned VALUES work, copied-query
ownership and schema-check provenance must retain their native boundaries.
No private prepared-source representation or complete phase repair is
accepted yet. Do not substitute a missing-table-only preflight, fake
expression/query identity, or unused public column arena.

The [current audit boundary](sqlite-reference-audit.md#current-open-boundaries)
records the remaining mechanism groups. These groups are not a claim that
all individual cases have been enumerated. IN query RHS and later SQL
features remain blocked by the open prerequisite work.

## Where each kind of information lives

| Information | Canonical entry |
|---|---|
| Required context, ownership and development rules | [Agent contract](../AGENTS.md) |
| Layering and dependency constraints | [Architecture](architecture.md), [DAG](dependency-dag.md) |
| Active node and prerequisites | [Manifest](../project/manifest.json), [graph](../project/module-graph.json), [progress](../project/progress.json) |
| Verification commands and evidence boundaries | [Verification](verification.md) |
| Decisions, rationale and supersession | [ADR navigation](adr/README.md) |
| Prior baseline instructions and public evidence | [Historical handoff through PR #260](history/development-through-pr260.md) |

Read the repository-required context and only the ADRs applicable to the
selected work; the chronological ADR archive is not a mandatory sequential
reading list. This handoff is an index, not another copy of those contracts.

## Resume safely

Use the existing checkout where available. On a new machine, clone the
repository and follow [build setup](../README.md#build) and
[verification](verification.md), including the platform-specific boundaries.
Use the committed workflow and pinned manifest to recreate tools and the
SQLite reference; do not copy old build caches or compiled libraries.

```sh
git switch main
git pull --ff-only
git status --short
python3 tools/project_graph.py validate
python3 tools/project_graph.py next-ready --json
```

Inspect and preserve local changes and stashes before switching branches.
Use the current main checkout. The approved PR #331-333 source references were
intentionally retired; the preserved stashes are not disposable source
branches. Keep future handoffs current when qualification or next work changes.
