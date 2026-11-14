# Current Development Handoff

Updated: 2026-11-14. Traceability: `implement-advanced-sql`, ADR-0106,
ADR-0111, ADR-0255, ADR-0256, ADR-0257, ADR-0258, ADR-0259, ADR-0260,
ADR-0261, ADR-0262.

This is the current portable starting point. The repository and linked
pull-request evidence are authoritative; an earlier machine's session
directory, binaries and absolute paths are not prerequisites.

## Qualified main and retained history

[PR #334's qualified main handoff](https://github.com/algorithm-apprentice/modern-sqlite/pull/334#issuecomment-6874592721)
records main `b53a9a36aaaf22d104c32f075cb10186bf4f9ad4`, tree
`9983e5306f3f3be8c951f8577729f81de27f81c8`, ledger410. Its original source CI
passed twelve contexts, full-production source quality completed all objects,
both library links and formatting, and actual-main CI passed all ten roles.
The normal squash preserves that reviewed tree.

The older Windows startup timeout and cancelled source quality run remain
failed observations. The accepted startup-only correction qualified a new
head; it did not replay those jobs or establish their historical causes.
The rejected direct-replay proposal sent no request.

[PR #333's predecessor handoff](https://github.com/algorithm-apprentice/modern-sqlite/pull/333#issuecomment-6857728951)
retains the earlier qualification lineage, including closure of PR #332's
delivery block and its failed original runs. Its diagnostic allowance
remains closed.

The approved source branches were retired with expected-OID guards; all four
stashes remain preserved. Retain the original failures and unexecuted
suffixes as failed evidence: later qualification does not establish their
cause. Do not reset main, reopen completed delivery controllers or reuse an
earlier diagnostic request allowance.

The active DAG node is still `implement-advanced-sql`; nodes 44-48 remain
blocked. Use [project progress](../project/progress.json) and the
[module graph](../project/module-graph.json) for node state, not historical
test totals or a previous session's todo count.

## Delivered prerequisites and next SQL boundary

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

[ADR-0259](adr/0259-canonical-ordinary-construction-events.md) is delivered:
wildcard function arguments use one zero-argument classification, and selected
ordinary construction shares the existing lexical event owner with VALUES.
Function constructors and parameter occurrences retain that native order
before later binding.

[ADR-0260](adr/0260-native-python-startup-budget.md) supplies a normal
configuration correction: this one Windows Python owner gets thirty seconds
for startup, retaining ten seconds after full module readiness. It does not
relax functional/child limits or explain the separate quality cancellation.
The corrective source and actual main are qualified as recorded above.
Private source preparation is the active PR #335 behavior batch; `NC_NoSelect`
remains a subsequent correction.
The established source prerequisites are:

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

[ADR-0261](adr/0261-canonical-table-source-definitions.md) is the accepted
internal prefix `1f686f0`, ledger411. It separates catalog/schema definitions
from executable publication and closes no behavioral difference by itself.
[ADR-0262](adr/0262-native-select-source-preparation.md) owns the dependent
correction: prepare sources, JOIN metadata and wildcard metadata before
names, with native pending-error guards, converted compound traversal and
detached VALUES first-producer entry. It retains no public placeholder IDs
or alternate column arena. The fourteen-case starting matrix's eight
error-owner differences now pass locally; final batch qualification and
delivery remain outstanding. Do not treat the draft or this local result
as qualified main.

After that batch, retain non-descending compound comparison, per-core
name/width/LIMIT ordering and later value-generation arbitration as open
work. Source preparation does not suppress query name resolution inside
ORDER trials or establish whole-generator parity.

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
Use the current main checkout. The approved PR #331-334 source references were
intentionally retired; the preserved stashes are not disposable source
branches. Keep future handoffs current when qualification or next work changes.
