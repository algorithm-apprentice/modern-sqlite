# ADR-0099: Preserved USING Wildcard Publication

- Status: Accepted
- Date: 2026-10-16

## Context

The implemented-engine reference audit's A2 reproduction distinguishes
generated wildcard references from explicit qualified references. For
RIGHT/FULL USING or NATURAL joins, `a.*` on the left of a preserving
boundary may publish the joined key even though `a.key` remains physical.
Modern currently restricts joined wildcard values to unqualified `*`.

Pinned SQLite generates an unqualified wildcard reference when its source
lies to the left of a RIGHT/FULL boundary and a later USING clause contains
that column name. `src/select.c:6015-6027,6358-6379` applies this before
ordinary name resolution. NATURAL clauses use their normalized USING names.
The boundary and the later USING clause need not be the same source.

A bounded native/current comparison on the existing independent audit image
covers two-source forms, NATURAL, later mixed boundaries, duplicate
qualifiers, schema qualification, and mixed column spelling/affinity.
Of 197 instances, 53 differ: 12 ordinary two-source, two NATURAL, 29 later
boundary, and ten mixed-name/type instances. These are overlapping instances,
not separate bugs.

The mixed-name probe also exposes a coupled publication rule: a wildcard
keeps its owning column's spelling even when its value resolves through a
RIGHT key from another source. Current unqualified RIGHT output can choose
the right column's spelling instead. Explicit-expression naming is distinct.

The canonical mechanisms already available are:

- immutable per-core source syntax and source states in declared order;
- `SourceJoinPreservesRight` for RIGHT/FULL classification;
- one source-column namespace with `merged_into` provenance and
  `joined_value` overrides, including normalized NATURAL/USING links;
- one `ResolveSourceName` operation for physical qualification, unqualified
  joined values, duplicate qualifiers, ambiguity, and rowid fallback;
- one joined coalesce builder and provenance validation;
- one result/definition vector and implicit metadata builder; and
- existing query-owner frames, expression properties, leaf discovery,
  consumers, failure handling, allocation sweeps, and native harness.

Begin after PR #208 at main commit
`b1d60e4dc7324f79aff3165c4830735e8f473e1b`, tree
`dab91619de35f297b28f81103d46a6171c1d86e9`. All nine exact-head gates
passed; the original unchanged model passed in 4.18 seconds under its
ten-second deadline. Node 43 remains active.

## Decision

### 1. Select generated qualification from the existing core metadata

For a qualified wildcard column, determine whether its owner is strictly
left of a RIGHT/FULL source and whether the same declared name participates
in a later normalized USING/NATURAL link. Both facts are required. Use the
current core's source order, existing join-kind predicate, and existing
right-column `merged_into` metadata; do not reconstruct USING comparisons
or allocate another namespace/name map.

Borrow the current core's source syntax at wildcard expansion if needed
for its join kinds. Validate correspondence with the already bound source
states through the repository's typed invariant errors. Add no persistent
per-source flag, owner copy, source registry, or rewritten syntax.

When both facts hold, resolve the generated name unqualified through the
same `ResolveSourceName` operation. It then selects the canonical joined
value and preserves ordinary ambiguity reporting. Do not bypass resolution
by selecting a convenient physical contributor or constructing a second
coalesce.

Otherwise retain the existing qualified generated-name resolution, including
duplicate qualifier groups, schema-table names, aliases, and unrelated-name
ambiguity. Explicit `a.column` binding is unchanged. The current unqualified
wildcard suppression and visible-column order remain unchanged.

### 2. Separate wildcard names from resolved value metadata

Keep every wildcard output name equal to the owning declared column name,
including its original spelling. The native generated expression's implicit
name is not a request to rename the wildcard after its selected value owner.
Use the existing metadata builder for declared type and expression properties,
then preserve the wildcard column name at this publication site only.
Honor its existing supplied generated-name argument for physical leaves as
well as coalesced values, rather than adding another naming implementation.

RIGHT wildcard values retain the selected right value's affinity, collation,
and declared type. FULL coalesces retain the canonical first-contributor
affinity/collation and no declared type. Physical explicit references keep
their own metadata and expression-derived names.

Keep the existing result-column/definition ownership, width-before-expression
admission, wildcard expansion count, hidden-rowid exclusion, and
`JoinedValueMatches` provenance validation. Do not suppress extra columns
from a qualified wildcard merely because their names are merged.

### 3. Preserve every consumer and boundary

The binder publishes the same existing expression alternatives and source
identities, so ordinary rows, ordering, DISTINCT/sets, compounds, grouping,
aggregate snapshots, scalar/EXISTS demand, and source capture inherit the
correct value without another execution mechanism.

Preserve actual core ownership and later USING/NATURAL effects; do not use
root metadata for a compound or nested core. Keep direct qualification,
duplicate qualifiers, no-later-USING and no-right-boundary controls explicit.
Complete LEFT/RIGHT/FULL preservation, ON/WHERE phases, replay, runtime
budgets, and source/temporary failure cleanup are unchanged.

Production changes belong only to canonical wildcard binding/publication.
No parser, planner, lowering, VM, storage, public API, query-instance feature,
or later-node behavior is admitted.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before production changes. First observe
a public-session wildcard red on the unchanged fixture, proving the merged
unmatched-right key and FULL type metadata while explicit `a.key` stays NULL.
Add binder shape/provenance/name/type controls before implementation.

Cover RIGHT/FULL and NATURAL, both source orders, later mixed boundaries,
same/different USING names, complete absent-side prefixes, duplicate/schema
qualifiers, unrelated-name ambiguity, mixed declared-name spelling/types,
collations, and no-boundary physical controls. Exercise all existing
consumers, actual compound/private owners, reset/rebind, source/spill
failures, and exhaustive allocation failure through current helpers.

Use the existing profile-validated native harness and unchanged per-corpus
cap. Register one bounded namespace shard through the same verify/regenerate
scripts, including both required control kinds. Reuse existing fixtures for
ordinary cases and add only an independently generated small metadata fixture
for unequal column spelling/types; preserve all old fixture and oracle bytes.
Record its SQL, native identity, and digest through the established fixture
pattern, not a new generator or compatibility layer.

Replay the 197 baseline instances and the frozen cross-feature comparison;
new accepted values, metadata, and prepare errors must match native without
new unrelated differences. Run the focused and complete fast tiers, required
compiler/sanitizer/static/native/performance gates, inherited work controls,
and a bounded grammar campaign. Preserve every old allocation ceiling,
workload, profile, resource control, threshold, and deadline.

Require GPT-5.6 Sol implementation and exact-tree review and all nine
new-head hosted checks before normal squash merge. Verify merged main and
clean only the confirmed merged branch. This correction does not complete
other audit findings, ADR-0092 behavior deliveries, or node 43.

## References

- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0078: USING and NATURAL Joined-Column Namespaces
- ADR-0079: RIGHT Join Matched-Row Replay
- ADR-0080: FULL Join Two-Sided Preservation
- ADR-0084: Bound Query Ownership for Nested SELECT
- ADR-0092: Semantic Query Instances for Result References
- [Implemented-engine reference audit](../sqlite-reference-audit.md), A2
- Pinned SQLite `src/select.c:6015-6027,6358-6379`
