# ADR-0078: USING and NATURAL Joined-Column Namespaces

- Status: Accepted
- Date: 2026-10-14

## Context

PR #188 completes executable INNER/LEFT/ON over the canonical plural-source
producer. ADR-0053 slice 5 still requires merged USING/NATURAL output before
RIGHT/FULL and measured parameterized/automatic access can close joins.

Existing source columns already own stable statement-global IDs, physical
source occurrence IDs, names, declared types, affinities, and collations.
Qualified and generated wildcard names share one resolver. Bound comparison
expressions already publish affinity/collation, and every consumer reads the
same accepted joined rows.

Pinned SQLite probes establish the less obvious namespace rules:

- USING suppresses the right named column from unqualified `*`, without
  changing left column order or suppressing qualified right columns;
- unqualified merged values in the admitted INNER/LEFT forms use the left
  column, including when LEFT publishes a NULL right row;
- equality uses ordinary three-valued comparison and left operand collation;
- a USING name chooses the first matching visible column in a left prefix,
  even when another left occurrence has the same name; those two visible left
  columns remain ambiguous to an unqualified reference;
- duplicate USING names are accepted, but implicit rowid spellings are not
  declared columns eligible for USING;
- duplicate qualified matches in the same merged group resolve to the first
  qualifying occurrence, while unrelated duplicate matches remain ambiguous;
- generated wildcard references use that same resolution, so `a.*` over two
  identically aliased NATURAL sources publishes two sets of left values,
  including a left value where the physical right row is NULL; and
- NATURAL uses every common declared name, adds no condition when there is no
  common name, and rejects an explicit ON or USING clause.

A second result-column store or a second generated-name resolver would copy
existing semantics and lose these interactions.

## Decision

### 1. Extend the existing source syntax

Each table source gains a NATURAL modifier and an optional owned USING clause
containing its exact span and ordered name spans. Keep source names, aliases,
join operators, ON expressions, and USING names in SQL order. Accept USING on
the already executable comma/CROSS/INNER/LEFT forms and NATURAL JOIN,
NATURAL INNER JOIN, NATURAL CROSS JOIN, and NATURAL LEFT [OUTER] JOIN.

Reject empty/malformed USING lists, repeated clauses, and mixed ON/USING syntax
without accepting a prefix. The existing parser column-list limit also bounds
USING name storage. Preserve an explicit unsupported boundary for RIGHT/FULL,
parenthesized sources, derived sources, and source functions.

The binder reports the pinned first-source USING error, missing declared
column error, and NATURAL-with-ON/USING error through the existing typed join
scope error. Parsing remains catalog-independent.

### 2. Publish a joined namespace through canonical column metadata

Extend `BoundSourceColumn` with an optional `merged_into` source-column ID.
The target is an earlier, visible column in the same core. It always points
directly to the visible representative, never to another suppressed column.
The existing per-core source-column range plus these immutable links is the
joined-output namespace:

- columns without a target are visible in unqualified lookup and `*`;
- each suppressed right column records its own physical provenance and the
  representative's left provenance through the link; and
- qualified lookup still considers every physical source column.

Do not allocate a parallel column container, value store, union-find, or
merged-column name map. Physical source ownership and record offsets never
change. Activating another query core selects its existing source-column range,
so compound ORDER BY does not need another namespace reconstruction.

Only qualified duplicate matches in the same merged group may share one
resolution. Keep the first qualifying physical column as the value; a unique
right qualifier must continue to read the right row, including its NULL
extension. Hidden rowid fallback remains separate and ambiguous as before.

Unqualified wildcard expansion suppresses linked right columns. Qualified
wildcards retain all matching source columns and use the same generated-name
resolver. A generated reference may resolve to another physical occurrence
only when both columns belong to the same merged group.

This package represents the admitted INNER/LEFT merged value as its ordinary
left source leaf. RIGHT/FULL coalesced values require their own executable
package and value contract; do not add unused expression variants now.

### 3. Bind one condition in the existing expression arena

After binding source descriptors and before result expressions, process joins
in declared order. Explicit USING searches only declared right columns and
the visible left prefix. NATURAL performs that same operation for each common
declared right name. Existing source names and result aliases do not become
USING names.

For each pair, bind the two canonical physical leaves and append ordinary
equality through the same comparison builder used by scalar SQL. Combine
multiple equalities through the same bound logical-AND builder. Retain the
result in `BoundTableSource::on`, not a second condition representation.
Explicit ON retains its alias-enabled binding phase and scope checks.

The normalized combined predicate obeys SQLite's 1,000-level expression-height
limit. Checking each source condition separately is insufficient: equality
terms from consecutive USING/NATURAL clauses are appended directly to the
original WHERE tree. Explicit ON contributes its own source-syntax subtree.
Qualified names include their dot-node height; parentheses and COLLATE are
transparent.
Share the existing syntax construction limit through the AST contract and
report an explicit typed binding error with SQLite's generic primary code.
Compute this bounded height while binding source conditions, without another
expression container or a recursive walk of synthesized conjunctions.

Logical planning, physical readiness, LEFT match-before-WHERE, NULL extension,
and every ordinary/aggregate destination consume the existing ON contract.
No VM instruction, cursor representation, runtime namespace, source-row copy,
or new join algorithm is introduced.

### 4. Preserve boundaries and resource behavior

Normal source metadata, qualification, wildcard naming, alias precedence,
collation, aggregate identity, default substitution, reset/rebind, and exact
error/resource cleanup remain canonical.

The inline provenance link adds no allocation to existing statements.
Generated USING/NATURAL expressions use the existing arena and its explicit
allocation failures. Keep previous exact allocation counts and frozen
performance inputs unless measured behavior necessarily changes.

## Verification

Observe a focused public-session USING rejection before implementation, then
cover merged projection, output metadata/order, physical right qualification,
NULL right rowid/defaults, equality affinity/collation, and natural NULL
comparison.

Pin duplicate names/lists/qualifiers, ambiguous prefixes, schema/wildcard
quirks, hidden rowid exclusion, malformed spans/clauses, alias/group/order
scope, compounds, all consumers, zero/early LIMIT, reset/rebind, and the
64-source boundary. Pin both sides of the combined predicate-height boundary
with multiple sources, WHERE, and explicit ON. Add module-owned allocation
sweeps and real source/spill failure cleanup using existing helpers.

Add independently generated pinned SQLite vectors, deterministic merged-output
models, and grammar-aware sanitizer fuzz. Run the focused and full fast tiers,
appropriate complete compiler/sanitizer tiers, production quality checks, and
unchanged archived work contracts. Review local design, core, and incremental
evidence commits before publishing one coherent slice and squash merging only
after all hosted gates pass.

### Closure evidence

The public-session test first failed at the unsupported USING boundary.
The completed feature passes 1,163 fast unit cases and all 1,209 cases in each
complete optimized and ASan/UBSan tier. The independent GCC tier passed 1,206
cases before the isolated height correction; its 23 focused namespace,
depth, model/fuzz, and allocation cases pass with that correction.

Thirty-three new pinned SQLite vectors bring the committed matrix to 197
comparisons and six unsupported boundaries. All 170 previous oracle records
remain identical as JSON data. The vectors pin merged metadata/order,
qualified NULLs, natural NULL comparisons, duplicate names/qualifiers,
ambiguous prefixes, wildcard expansion, and explicit error boundaries.
Independent models exercise ordinary and identically aliased merged sources.
The grammar-aware sanitized SQL campaign completed 20,000 inputs.

Normal and zero-limit paths cover 64 sources. Memory/file parity includes a
1,024-row USING product and proves genuine spill by checking new file
acquisition. Source-read and PMA-flush failures preserve typed errors, exact
cleanup, and reset recovery. Grouped defaults, reset/rebind, completed-row
limits, sets, VALUES cores, and demand-one reuse the existing consumers.
Module-owned allocation sweeps cover generated names and conditions; previous
exact allocation budgets and all 42 archived runner/work contracts pass
without changing frozen inputs.

Incremental review found a genuine combined-height discrepancy after the core
was green: two sources with 999 duplicate USING names prepare successfully,
but 1,000 names exceed SQLite's limit; three sources fail at 500 names per
join. The independent red case preceded the correction. Forty-eight native
boundary probes now agree for WHERE constants, qualified names, arithmetic,
functions, COLLATE, and ON before/after generated terms. Persistent unit cases
pin those source-order and transparent-wrapper rules under optimized and
sanitized execution.

Direct design, core, and correction review found no unresolved issue. The
implementation retains one column namespace mechanism, one comparison builder,
and one existing ON producer; it adds no runtime join interpreter or alternate
row/value container.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0027, ADR-0028, ADR-0033, ADR-0034, ADR-0035, ADR-0036,
ADR-0053, ADR-0072, ADR-0073, ADR-0075, and ADR-0077.
