# ADR-0073: Comma and CROSS Join Execution

- Status: Accepted
- Date: 2026-10-13

## Context

ADR-0072's explicit source occurrence identities are merged through PR #183.
ADR-0053 slice 5 can now extend the single-source namespace and source producer
without confusing same-table aliases, rowid owners, or compound-arm scopes.

The first executable join package is the comma/CROSS product. INNER/LEFT
conditions, merged NATURAL/USING output, automatic indexes, and aggregate
consumption of joined rows are subsequent coherent packages in this slice.
This package retains every existing nonaggregate row destination: projection,
DISTINCT, ordering/top-N, compound streams/sets, and demand-one output.

Pinned SQLite uses the source occurrence namespace in `src/resolve.c:278-804`,
left-to-right source syntax in `src/parse.y`, nested WHERE loops in
`src/wherecode.c`, and source-order wildcard publication in `src/select.c`.
No new record, sorter, membership, value, function, cursor, or executor
mechanism is necessary.

Independent probes on the pinned profile establish:

- with two rowid sources, unqualified hidden rowid is ambiguous;
- real columns are searched across all matching sources before hidden rowid
  fallback, including sources with duplicate aliases;
- a qualified wildcard expands every matching source, while an individual
  qualified column is ambiguous only when multiple matching sources expose it;
- wildcard-generated qualified columns still undergo ordinary resolution, so
  duplicate qualifiers exposing the same column produce an ambiguity error;
- aliases hide the original source name;
- source-free volatile WHERE predicates run once per outer row, not once per
  Cartesian result;
- predicates depending on the outer source run before the inner loop;
- predicates depending on both sources run after both are positioned; and
- OFFSET skips projection, and LIMIT zero bypasses sources and callbacks.

For example, a two-row outer and three-row inner CROSS product calls
`tick()` in WHERE twice, `tick(outer_column)` twice, and `tick(inner_column)`
six times. `SELECT tick(x*10+y) ... LIMIT 1 OFFSET 2` projects only the third
accepted combination. These are execution contracts, not optional tuning.

## Decision

### 1. Publish one plural source representation

Replace the optional AST FROM source with one ordered source vector. Each
source retains its existing qualified name, alias, and source span plus the
preceding operator kind and span: first, comma, or CROSS JOIN. Never retain an
optional single-source compatibility field beside the vector.

Enforce SQLite's 64-source join bound. The first source has no preceding
operator; subsequent sources have a nonempty preceding operator span in SQL
order. Validate names, aliases, operator values, source containment, and
clause ordering through the existing immutable AST boundary.

Accept comma and `CROSS JOIN` between ordinary catalog or synthetic schema
sources. Parenthesized sources, subqueries, table functions, INDEXED BY,
ON/USING, and other join keywords remain explicit unsupported syntax in this
package. No accepted prefix is returned.

Binding publishes one ordered source vector per SELECT core. Each occurrence
retains its ADR-0072 identity and its statement-global source-column range.
The existing core-wide source-column range remains the total of those ranges.
Single-target mutations use the same canonical binding namespace with one
source. Saved core clause scopes retain their original occurrences.

### 2. Resolve the namespace over actual matches

Name resolution searches all applicable source occurrences for declared
columns first. Exactly one match binds its global column identity; multiple
matches return `kAmbiguousColumn` with SQLite-compatible detail. Qualified
lookup restricts candidate occurrences by schema and alias/name but does not
reject duplicate aliases before testing actual columns.

Only when no declared column matches may a hidden rowid spelling select a
rowid-capable candidate. Exactly one such candidate binds its explicit owner;
multiple candidates are ambiguous. WITHOUT ROWID tables are not candidates.
Source columns still precede aliases in alias-enabled clauses. DQS and
TRUE/FALSE fallback happens only after source lookup produces no match, never
after ambiguity.

Unqualified `*` expands sources in declaration order and their columns in
declaration order. Qualified `source.*` expands every matching occurrence in
that same order. Each generated schema/source-qualified column goes through
the same canonical resolver. Duplicate aliases with disjoint column names can
expand successfully; a common column is ambiguous rather than silently bound
to the wildcard's currently visited source. Preserve schema-table legacy
wildcard qualification and ordinary direct-column/rowid names, declared
types, affinities, and collations.

The plain comma/CROSS product has no merged columns. Later NATURAL/USING
packages extend this canonical publication with their admitted merged-output
semantics; they must not create a second base-source namespace.

### 3. Keep semantic plans and execution choices separate

Logical plans add a left-associated product node over the existing source
scan nodes. It carries source syntax, not cursors or loop state.

Physical plans select a bounded nested-loop product in declared source order,
using the existing table/primary-index scan capability for each source.
CROSS order is a barrier. Comma sources are not reordered in this correctness
baseline; later accepted planning may reorder eligible inner sources.
Unordered SQL row order is not promoted into a compatibility guarantee.

The optimizer splits WHERE through its existing conjunct/guard machinery.
Source-independent deterministic guards retain their pre-open phase. Each
remaining predicate publishes the earliest declared loop source at which all
its column/rowid dependencies are available. A source-free volatile predicate
belongs to the outer loop. Retain original conjunct order within each phase
and existing lazy expression semantics. Lowering consumes this schedule; it
does not re-run dependency or name analysis.

No join rowid/index constraint selection, automatic index, hash join, join
reordering, or sort-elimination claim is added without the later measured
mechanism. Single-source access planning remains unchanged.

### 4. Emit verified nested loops into existing destinations

Each source descriptor uses the existing read-cursor construction. One
canonical lowering map connects a bound source occurrence to its cursor and
rowid field, beside the existing statement-global column/field map.
Expressions read their actual owner's cursor, never a shared "current table."
No binder ID or physical plan enters the VM.

Open each selected source cursor once after the LIMIT/guard bypass, matching
SQLite's pre-loop cursor acquisition. Position one source, evaluate its ready
predicates, rewind the next already-open source, then send complete rows to the
existing streaming, ordering, materialized, or demand-one destination. Inner
exhaustion returns that cursor to its open/unpositioned state before outer
advancement; it does not reopen or reparse the root for every prefix. Empty
sources and rejected prefixes do not evaluate later predicates or projection.
Result suspension retains the positioned prefix.

Preserve destination schedules, including DISTINCT membership, top-N
key/admission/payload order, OFFSET projection skipping, compound output,
and demand-one early stop. LIMIT zero remains the pre-open bypass. A
consumer-requested stop closes every selected cursor exactly once before
joining the closed completion edge; natural exhaustion and early stop have
separate verified states. All descendants are open/unpositioned at parent
advancement and natural source completion. Advance/Next blocks are emitted
only when predicate rejection or child exhaustion can reach them. In
particular, an always-stop demand-one row consumer emits no deepest Next tail
unless a ready predicate may reject that row.

All cursor errors propagate through the existing typed VM/session failure and
cleanup. Reset, rebind, reprepare, finalize, and destruction retain their
existing ownership rules. No raw owning pointer or recursive runtime
executor is introduced.

### 5. Bound the package rather than accept partial support

Joined aggregate/grouping queries return an explicit unsupported-feature
binding error in this package. Existing single-source aggregates remain
unchanged. The next package connects joined rows to the same aggregate
consumer before slice 5 can complete. INNER/LEFT and ON/USING grammar also
remain explicit boundaries until their executable packages merge.

This is temporary declared scope, not a compatibility shim, silent fallback,
or a new independent aggregate implementation.

## Verification

Begin with focused public-session red behavior for a two-source product.
Cover parser/AST ownership and malformed operator/name/64-source boundaries;
alias, duplicate-name, rowid, schema, WITHOUT ROWID, wildcard, metadata, DQS,
and ambiguity rules; planner occurrence/schedule identities; and verified
empty/one/many-source loops.

Add pinned differential rows/types/metadata and probe schedules, three-source
and same-table models, normal and early-stop destination coverage,
memory/file ordering and membership parity, reset/rebind, every new allocation
boundary, and injected cursor read failure. Existing aggregate, compound,
mutation, allocation, fuzz, and archived performance contracts remain green.
Do not change frozen workload or baseline inputs.

Memory/file parity uses a 32-by-32 source product and a 32 KiB file budget,
which forces genuine sorter spill while leaving room for simultaneous
positioned consumers. Very small file budgets may return the existing explicit
`kTooLarge` when a positioned drain occupies the admission budget and a new
ordinary owner cannot reclaim memory. Pin that error and complete cleanup
separately; do not silently waive ADR-0058's ordinary-growth bound or broaden
its bounded drain exception.

The source-to-cursor map adds one allocation to each source-backed lowering
fixture, including the unchanged single-source consumers; source-free and
mutation fixtures retain their original counts. Repin these exact tested
counts rather than loosening their budgets. Aggregate DISTINCT and ordered
aggregate UNION ALL lowering become 79 and 85 allocations respectively.

Record fixed-work loop evidence for the correctness baseline. The join
scaling baseline and automatic transient indexes remain ADR-0053's next
performance package; this package performs no speculative speed tuning.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0028, ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053,
ADR-0054, ADR-0056, ADR-0072.
