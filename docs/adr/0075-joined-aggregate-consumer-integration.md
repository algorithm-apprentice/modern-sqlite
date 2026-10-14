# ADR-0075: Joined Aggregate Consumer Integration

- Status: Accepted
- Date: 2026-10-13

## Context

ADR-0073's comma/CROSS producer is merged through PR #184. It preserves plural
binding scopes, actual source ownership, earliest-ready WHERE predicates,
source cursor acquisition, and the existing SELECT row destinations.
Joined aggregates remain an explicit binding boundary until this package.

The canonical aggregate implementation already owns invocation deduplication,
FILTER, DISTINCT, local ordering, empty finalization, representative rows,
group sorting, HAVING, completed-row destinations, and cleanup. A joined row
is another input to those same contracts, not a reason to build a second
aggregate or grouping implementation.

The remaining ownership limitation is lowering's register lookup: ordinary
retained columns use a statement-global array, but rowid has one optional
register. Multiple joined sources require distinguishing each owner's rowid,
including its INTEGER PRIMARY KEY alias.

## Decision

### 1. Admit joined aggregation through existing binding

Remove ADR-0073's explicit joined-aggregate rejection only when the complete
compiler/session path executes it.

Retain the existing clause resolution, aggregate legality, invocation order,
collation, retained/representative leaf analysis, and source occurrence
identities. No new function state, value type, syntax clause, or joined-output
container is introduced.

### 2. Select global or sorted grouping for joined rows

Joined global aggregates produce at most one completed row, including empty
input. Joined grouping initially uses the existing stable group sorter.

Do not pretend the first source's access, rowid eligibility, or delivered
order describes the Cartesian producer. Its logical/physical source nodes
remain the existing declared-order product. Single-source indexed MIN/MAX and
ordered grouping remain unchanged.

The joined WHERE producer retains ADR-0073's guard and per-source residual
schedule. Aggregation never pushes invocation arguments into an earlier
source loop. GROUP BY and all invocation work consume accepted complete
joined rows. No hash aggregation, joined index probe, automatic index, or
ordered-group optimization is added.

### 3. Use one canonical retained-leaf register map

Replace lowering's separate retained-column array and single rowid register
with one compact `BoundSourceLeafIdentity`/`RegisterId` binding vector for each
retained-row role: global representative, grouped representative, or sorted
input.

Reuse ADR-0072's normalization. Same-owner INTEGER PRIMARY KEY and hidden
rowid share a binding; another owner's rowid does not. Ordinary columns retain
their statement-global identities. Each role owns one mapping container and
one allocation-free lookup, not parallel per-feature column/rowid maps.

The existing register pools and sorter-record layouts still own values.
This mapping changes only semantic lookup and must preserve single-source
representative, affinity, and ownership behavior.

### 4. Generalize the existing producer, not the aggregate lifecycle

Extract the existing bounded joined-loop emitter's row callback so the same
source traversal can feed either the ordinary SELECT destination or an
aggregate input consumer. Retain one source-loop, cursor, and ready-predicate
implementation.

The aggregate core:

1. retains the outer statement LIMIT-zero bypass;
2. initializes existing global state or grouped sorter/invocations;
3. requests complete accepted rows through the canonical joined producer;
4. calls the existing global step/representative or grouped-row collector;
5. closes all source cursors before completion; and
6. invokes the existing finalization or group-sorter drain and completed-row
   destination.

A false guard or empty source supplies zero input rows but still finalizes a
global aggregate. Sorted grouping produces no groups for empty input.
FILTER/argument skipping and local-order drain are unchanged.

Completed aggregate rows reuse streaming, ordering/top-N, DISTINCT,
compound sets, and demand-one output. No accepted join row is projected or
counted twice. HAVING and output LIMIT/OFFSET remain after group completion;
they never truncate required source accumulation.

Every row-destination router gives aggregate metadata precedence over join
metadata. When a core has both, enter `EmitAggregateCore` and invoke the
joined producer only as its input. Materialized and demand-one compound arms
must not route the core through ordinary joined projection.

Natural completion and consumer stop retain separate verified cleanup.
Source collection for joined sorted grouping completes before group drainage,
so a later consumer stop never closes source cursors twice. Error, reset,
rebind, finalize, and destruction use the existing shared cleanup.

### 5. Keep the next mechanisms separate

This package adds no INNER/LEFT, ON/USING, source reordering, parameterized
join access, recursive executor, or persistent mutation. Those remain
ADR-0053 slice 5's subsequent reviewed packages.

## Verification

Observe focused public-session red behavior before enabling joined aggregates.
Cover global and grouped input, empty/guard-rejected products, distinct source
rowid/PK aliases, representative rows, FILTER, DISTINCT, local ordering,
HAVING, completed-row ORDER/LIMIT, and every existing destination.
Pin mixed single-source/joined aggregate set and demand-one compounds in both
arm orders, including GROUP-BY-only cores, so routing cannot bypass the
aggregate lifecycle.

Add independent pinned SQLite rows/types/metadata and callback schedules,
vector-model and grammar-aware fuzz coverage, exhaustive new-allocation OOM,
memory/file spill parity, injected-source/spill failure cleanup, reset/rebind,
and zero-LIMIT bypass.

Start single-source refactoring from the green PR #184 baseline. Preserve or
explicitly repin exact allocation counts according to the canonical mapping
change, without relaxing budgets or frozen benchmark inputs.

The compact role maps remove empty statement-wide column-map allocations in
advanced aggregate contexts. The measured exact integration counts become
77 for aggregate DISTINCT and 82 for ordered aggregate UNION ALL (previously
79 and 85 under ADR-0073); ordinary single-source fixture counts are preserved.

Review design and implementation, run the existing compiler/sanitizer/quality
and work-contract gates, and merge before the next production package.
`implement-advanced-sql` remains in progress.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0057, ADR-0058, ADR-0060, ADR-0061, ADR-0068, ADR-0069,
ADR-0072, and ADR-0073.
