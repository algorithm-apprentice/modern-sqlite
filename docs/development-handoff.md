# Current Development Handoff

Updated: 2026-11-13. Traceability: `implement-advanced-sql`, ADR-0106,
ADR-0111, ADR-0255, ADR-0256.

This is the current portable starting point. The repository and linked
pull-request evidence are authoritative; an earlier machine's session
directory, binaries and absolute paths are not prerequisites.

## Qualified SQL baseline

[PR #329](https://github.com/algorithm-apprentice/modern-sqlite/pull/329) is
delivered as main `76bfd939b11046d61460bef3c893adbd509f0bae`, tree
`9ed78237252b5ddb4221139bd5214d8a74fa67a1`, delivery ledger382.
Its [qualified handoff](https://github.com/algorithm-apprentice/modern-sqlite/pull/329#issuecomment-6816452485)
records independent acceptance, exact-source qualification and completed
source-branch retirement. Later qualified main deliveries supersede this
baseline; never reset current main to an older handoff.

The actual-main qualification combines six original successful executions
with four separately reviewed recovery-conditioned Linux executions.
Earlier Linux dependency-install failures and the original PR macOS timeout
remain historical failures, not successful runs or established causal
repairs. Their request allowances and completed delivery controllers are
spent. Do not restart that delivery or reuse an earlier request approval.

The active DAG node is still `implement-advanced-sql`; nodes 44-48 remain
blocked. Use [project progress](../project/progress.json) and the
[module graph](../project/module-graph.json) for node state, not historical
test totals or a previous session's todo count.

## Maintenance delivery and next SQL boundary

[PR #330](https://github.com/algorithm-apprentice/modern-sqlite/pull/330)
implements [ADR-0256](adr/0256-executable-change-aware-verification.md):
consistent documentation entry points, canonical verification-plan execution,
exact Python harness-test ownership, and truthful changed-TU analysis reports.
This is a tooling delivery, not a new SQL or node-completion claim. Existing
assertions, workflow deadlines and repository protection are unchanged.
It merged as main `9a648c6da2886445f0f8e50cd6c829b1940db11f`, delivery
ledger392.

Its first SQL-plan consumer exposed a report-role defect: the parent treated
a successful child's newly written report as a changed immutable input.
ADR-0256's follow-up distinguishes explicit report outputs from command inputs
without weakening source checks or prior-receipt validation. The original
parent failure remains failed; it cannot qualify the pending SQL slice.

After this bounded verification prerequisite, resume complete private source
preparation and `NC_NoSelect` name-resolution behavior. The established
prerequisites are:

- [ADR-0254](adr/0254-canonical-source-binding-scope.md): one active/saved
  source-state owner; delivered by PR #328.
- [ADR-0255](adr/0255-canonical-source-namespace-operations.md): canonical
  wildcard and USING enumeration with immediate borrowed column views;
  delivered by PR #329 without changing preparation-phase behavior.

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
Use the current main checkout; the older source branch for PR #329 was
intentionally retired. Keep future handoffs current when a delivery changes
the qualified baseline or next work.
