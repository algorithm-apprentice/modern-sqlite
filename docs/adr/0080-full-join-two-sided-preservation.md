# ADR-0080: FULL Join Two-Sided Preservation

- Status: Accepted
- Date: 2026-10-14

## Context

PR #190 provides canonical RIGHT matched-key replay, while ADR-0077 provides
LEFT per-prefix match tracking and typed NULL extension. FULL must combine
these accepted mechanisms rather than introduce another executor.

Pinned SQLite probes establish that:

- FULL emits matching rows and preserves each unmatched side exactly once;
- ON success is recorded before WHERE or later joins reject an output;
- a virtual unmatched-left row must not enter the right matched-key relation;
- FULL NULLs the complete missing prefix during right-side replay;
- merged USING/NATURAL values are lazy coalesces, have no declared type, and
  retain the first contributor's affinity and collation; and
- qualified duplicate names coalesce only matching members of their merged
  group, not a later source with another qualifier.

`src/select.c:594-649`, `src/resolve.c:435-470,755-780`, and
`src/expr.c:45-90,245-280` define the pinned prior-key predicate normalization,
scoped FULL output coalescing, and deferred first-argument properties.

## Decision

### 1. Accept one complete FULL surface

Add FULL JOIN, FULL OUTER JOIN, and their NATURAL/USING forms only with
end-to-end execution. Preserve all existing source, column, expression,
ownership, and explicit error boundaries.

Introduce canonical syntax-kind predicates for left and right preservation.
Use them in binding, planning, and lowering so FULL cannot accidentally be
classified as INNER, only LEFT, or only RIGHT on another surface.

As with RIGHT, a core containing FULL rejects forward ON references throughout
that core. Unrelated compound arms retain their existing admitted behavior.

### 2. Compose one physical-candidate transition

For a FULL source, evaluate its own ON once per physical candidate. On
success, set the existing per-prefix left-preservation flag and insert the
existing right identity into the canonical matched-key relation before WHERE
or suffix work runs.

Bind one accepted-row label after both publications. Physical matches,
unmatched-left NULL-right rows, and unmatched-right NULL-prefix replay all
enter that same body:

- physical exhaustion with no ON match publishes one NULL right row and
  jumps past ON and matched-key insertion;
- the virtual Next EOF consumes that extension exactly once; and
- right replay retains its mode-dispatched exhaustion edge, so it does not
  trigger the per-prefix left extension a second time.

Keep ordinary and replay continuation ownership, source acquisition, key
identity, exact close paths, memory budgets, and typed failures unchanged.
Multiple FULL/RIGHT boundaries replay in declared source order, allowing an
earlier replay to match later preserved sources.

No new bytecode, verifier relaxation, runtime plan interpreter, recursive
suffix copy, or alternate matched-row store is introduced.

### 3. Publish FULL merged values in the existing arena

Retain the canonical visible source-column slot and physical provenance links.
Its FULL joined-value override refers to the existing
`BoundCoalesceExpression`, built from the current published left value and
the new physical right leaf. Bind subsequent normalized USING predicates
through ADR-0079's all-prior-physical-contributor rule.

Use one joined-coalesce builder for output and scoped duplicate-qualifier
resolution. It preserves the first argument's affinity and collation, unlike
an ordinary user-spelled coalesce with no implicit affinity.

Unqualified lookup reads the full merged value. A unique physical qualifier
still reads only its actual source. Within one matching qualifier group,
INNER/LEFT retains the current value, RIGHT replaces it, and FULL coalesces
the matching values. A RIGHT replacement clears any previous scoped coalesce.
Unrelated duplicate groups remain ambiguous.

Unqualified wildcards publish a synthetic merged expression when required,
while preserving visible slot order and column names. Qualified wildcards
reuse the canonical source-name resolver. Validate that a generated merged
value contains only leaves from the expected provenance group, including
aliases and coalesces.

Coalesced output metadata has no declared type. A scalar `q.id` keeps its
expression-derived name when it becomes a scoped coalesce; wildcard output
keeps the declared column name. Ordinary and RIGHT-only metadata remains
unchanged.

### 4. Keep outer barriers and failures canonical

FULL participates in the same right-preservation predicate floor as RIGHT.
Own ON conditions remain physical-candidate work; ordinary WHERE and suffix
conditions consume the accepted logical row after preservation bookkeeping.
Keep the existing combined predicate-height check, including synthesized
prior-key coalesces.

New tests use the canonical independently selected OOM runner and complete
allocation partitions when required. Do not reduce failure ranges, alter
frozen performance inputs, or weaken a timeout/corruption check to admit this
feature.

## Verification

Observe a public-session FULL rejection before implementation. Cover matches,
unmatched left/right/complete prefixes, both-empty and one-empty inputs,
WHERE-rejected matches, mixed and repeated FULL/RIGHT/LEFT/INNER chains,
rowid and WITHOUT ROWID identities, and one shared readable body through the
64-source boundary.

Pin merged and physical metadata, first-key affinity/collation, qualifier
subgroups and wildcards, subsequent USING predicates, grouping/modifiers,
DISTINCT/sets/compounds/VALUES/demand-one, zero/early LIMIT, reset/rebind,
source and matched-relation/PMA failures, exact cleanup, every new allocation
boundary, independent models, and grammar-aware sanitizer fuzz.

Add pinned SQLite differential vectors without changing old outcomes.
Review design, implementation, and incremental corrections locally, publish
one coherent slice, and squash merge only after all required local and hosted
gates pass. Join access/scaling and subsequent SQL slices remain separate.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0056,
ADR-0058, ADR-0072, ADR-0073, ADR-0075, ADR-0076, ADR-0077, ADR-0078,
and ADR-0079.
