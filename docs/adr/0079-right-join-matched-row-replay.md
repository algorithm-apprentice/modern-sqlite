# ADR-0079: RIGHT Join Matched-Row Replay

- Status: Accepted
- Date: 2026-10-14

## Context

PRs #188 and #189 provide conditional INNER/LEFT execution and canonical
USING/NATURAL output. The next prerequisite-ready part of ADR-0053 slice 5
preserves unmatched right rows. FULL later combines this capability with the
existing unmatched-left transition.

Existing capabilities already provide:

- occurrence-owned rowid and declared-column identities;
- read cursors for ordinary, schema, and WITHOUT ROWID sources;
- typed NULL-row publication and exact cursor invalidation;
- spill-capable keyed relations and typed insert/contains operations; and
- one joined row producer consumed by projection, ordering, membership,
  compounds, demand-one, and aggregation.

Pinned SQLite probes establish that an unmatched RIGHT row NULLs the complete
left prefix, not just its immediately preceding table. ON matches must be
recorded before WHERE or later joins can reject output. Forward ON references
are rejected throughout a query core containing RIGHT, including an INNER
clause before or after the RIGHT boundary.

RIGHT USING output takes the right value, affinity, collation, and declared
type. A unique left qualifier still reads its physical left column. Duplicate
qualifiers in one merged group select the last matching RIGHT occurrence,
not a right value outside the qualifying group.

## Decision

### 1. Deliver executable RIGHT as one bounded slice

Accept RIGHT JOIN, RIGHT OUTER JOIN, and their NATURAL/USING forms only when
the complete producer reaches every existing consumer. Keep FULL explicitly
unsupported until its later executable package. Preserve the 64-source,
column, syntax, and combined-predicate-height limits.

A query core containing RIGHT rejects forward ON source references with the
pinned typed scope error. This restriction is core-local; an unrelated compound
arm does not change an existing INNER arm's admitted scope.

### 2. Preserve joined-output order and physical provenance

Extend the canonical visible source-column representative with an optional
joined value expression. RIGHT stores the already bound right leaf from its
USING equality; ordinary columns and existing INNER/LEFT forms retain their
current value when no override is required.

Later USING comparisons bind the current joined value of their left prefix.
Unqualified names and wildcard publication read that value while retaining
the established visible slot order. Unique qualified names still bind their
actual physical source. The existing generated-name resolver handles
qualifying merged duplicates; unrelated matches remain ambiguous.

Reuse ordinary bound leaves, alias references, metadata, and expression
properties. Do not add a second column/value container or an unused FULL
coalescing variant in this package.

### 3. Keep matches in the canonical keyed relation

Each RIGHT occurrence owns one statement-lifetime matched-key relation.
Use a source rowid for ordinary/schema sources and the complete canonical
primary key for WITHOUT ROWID sources. Reuse catalog primary-key order,
collation, record encoding, and source field mappings.

Record a right key immediately after its own ON succeeds, before WHERE,
later ON clauses, or descendants reject an output row. Duplicate keys use
the existing keep-existing insertion contract.

Open each selected source cursor and matched-key relation once after the
existing LIMIT/guard bypass. The relation remains in its ordinary writing/
membership state; no separate match bitmap, hash table, bloom filter, or
source-row copy is introduced.

### 4. Replay unmatched right rows in declared source order

First run the existing declared-order physical producer and accumulate
matches. After the complete left-prefix producer exhausts, revisit each
RIGHT source in source order:

1. publish typed NULL rows on every preceding source cursor;
2. rewind the right source and skip keys already in its matched relation;
3. enter that source's existing accepted-row body without reevaluating its ON;
4. run the same suffix and row consumer; and
5. clear the synthetic prefix before proceeding to the next replay.

An earlier replay can match a later RIGHT occurrence; its key must enter that
later relation before the later replay begins.

Share each readable suffix body through bounded labels and a per-RIGHT
normal/replay register. Its advance and exhaustion edges dispatch to the
appropriate physical or replay continuation. Avoid recursively copied suffix
bodies, new VM subroutines, or a runtime physical-plan interpreter.

Per-emission continuation labels are compiler-owned state with a different
lifetime from immutable source metadata and reusable register layouts. Keep
one bounded continuation collection only when RIGHT is present; preserve
existing allocation counts for statements without RIGHT.

### 5. Respect outer barriers and exact lifecycle

WHERE cannot execute before a RIGHT boundary that may replace a referenced
left prefix with NULL. Publish a conservative physical readiness floor at the
last relevant RIGHT boundary. An INNER/CROSS/comma ON after that boundary
also waits until the corresponding logical prefix is available. Own RIGHT
and LEFT ON conditions retain their physical-candidate phases.

Unmatched replay skips earlier prefix conditions but executes ordinary WHERE
and all suffix conditions. Match publication is not based on emitted-row
count. Early LIMIT/demand-one, empty sources, suspension, reset/rebind,
errors, and destruction use the existing exact close/failure paths for all
cursors and relations.

Matched-key growth obeys the shared statement memory/file budget. Report
ordinary budget and I/O failures explicitly; do not broaden a drain exception
or waive corruption checks to complete replay.

## Verification

Begin with a public-session RIGHT rejection before implementation. Pin empty
left/right inputs, matches, unmatched complete prefixes, rejected matched rows,
mixed LEFT/RIGHT/INNER chains, multiple RIGHT replays, source-owned rowid and
WITHOUT ROWID keys, and merged metadata/qualification/wildcard behavior.

Cover the shared body and all destinations, zero/early LIMIT, reset/rebind,
source/ledger/spill failures, exact cleanup, every new allocation boundary,
and bounded 64-source compilation. Add independent native vectors, models,
and grammar-aware fuzz.

Review design, core, corrections, and incremental evidence locally. Keep
existing allocation budgets and frozen performance inputs intact where
behavior is unchanged. Publish one coherent slice and squash merge only after
all required local and hosted gates pass.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0056,
ADR-0058, ADR-0072, ADR-0073, ADR-0075, ADR-0076, ADR-0077, and ADR-0078.
