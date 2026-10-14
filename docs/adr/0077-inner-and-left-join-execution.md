# ADR-0077: INNER and LEFT Join Execution

- Status: Accepted
- Date: 2026-10-14

## Context

The plural source producer, aggregate consumers, and typed NULL-row publication
are merged through PRs #184, #186, and #187. ADR-0053 slice 5 can now accept
conditional INNER and LEFT joins without a second executor or row store.

Pinned SQLite probes establish:

- bare JOIN and INNER JOIN select matching rows;
- LEFT JOIN preserves a left prefix with one NULL right row only when no ON
  candidate matches;
- a matched row rejected by WHERE does not turn into an unmatched row;
- ON can resolve nonaggregate result aliases, with source columns first;
- INNER/CROSS/comma ON may reference a later source, while a LEFT ON reference
  to its right is a prepare error; and
- CROSS and comma sources may carry ON, while the first source cannot.

The existing optimizer already visits bound source dependencies and publishes
earliest-ready WHERE placement. ON scope validation now requires the same
semantic traversal in binding. Share that implementation, not another
column/rowid dependency algorithm.

## Decision

### 1. Extend the canonical source syntax and namespace

Extend `SourceJoinKind` with INNER and LEFT, and add an optional ON expression
to each existing table source. Accept bare JOIN, INNER JOIN, LEFT JOIN, and
LEFT OUTER JOIN, retaining operator/source/condition spans in SQL order.
Comma/CROSS ON uses the same syntax contract.

Reject first-source ON, repeated ON, missing expressions, malformed operator
sequences, and the existing unsupported USING/NATURAL/RIGHT/FULL,
parenthesized-source, function, and subquery boundaries explicitly.
The 64-source limit remains unchanged.

Bind ON with ordinary source columns and nonaggregate result aliases available.
It cannot establish aggregate-query status or contain aggregate calls.
LEFT ON cannot reference a later occurrence; report the pinned prepare error.
INNER/CROSS/comma ON retains SQLite's admitted forward source resolution.

One source condition belongs to the existing bound source descriptor and
expression arena. There is no parallel joined-output namespace or copied AST.

### 2. Share dependency traversal and publish condition placement

Reuse one binder-level bound source-owner traversal from both binding and
optimization. It recognizes canonical source leaves, aliases, and all existing
scalar/lazy expression variants without retaining environment pointers or
allocating a dependency container.

Logical join nodes retain the source's ON expression. Physical join nodes
publish that expression and its earliest ready source. An INNER/CROSS/comma
condition can run when all referenced sources are available; a source-free
volatile condition belongs to the outer loop. LEFT ON is evaluated only for
physical candidates of its own right source, even when its expression uses
only earlier sources. Its readiness never crosses that NULL-extension
boundary.

WHERE retains its existing per-source schedule but executes after an own LEFT
match has been recorded. Conditions and residual predicates are not silently
folded into projection, aggregate arguments, or a VM plan interpreter.

### 3. Extend one bounded nested-loop emitter

All source cursors retain their current one-time acquisition and shared
descriptor/field/rowid mappings.

An INNER candidate evaluates ready conditions and proceeds only when accepted.
For each LEFT right-source entry:

1. reset one existing-register match flag before positioning;
2. evaluate ON for each physical candidate;
3. set the flag immediately on ON success, before WHERE or downstream joins;
4. send that accepted logical row through the existing shared body; and
5. on physical exhaustion, publish one typed NULL row only if the flag is
   still false, mark extension consumed, and enter the same body.

The NULL row skips its own ON condition, but ordinary WHERE and downstream
conditions still apply. `NullRowInstruction` plus virtual Next EOF returns the
source to open/unpositioned state. This shares one readable body and avoids
exponential copies for a chain of LEFT joins.

Natural exhaustion, rejected prefixes, LIMIT/demand-one stopping, suspension,
and failure preserve the existing exact cursor/temporary lifecycle. A match
flag describes ON matching, not emitted-row count or WHERE acceptance.

### 4. Preserve every existing consumer

Ordinary projection, DISTINCT, order/top-N, compounds/sets, demand-one,
global aggregation, sorted grouping, representatives, modifiers, HAVING, and
completed-row LIMIT/OFFSET consume the same accepted logical source rows.
NULL right columns/rowid pass through existing SQL affinity/collation/value
semantics; missing-field defaults do not replace NULL extension.

Unordered SQL row order remains unspecified. The declared-order nested-loop
baseline retains CROSS and LEFT barriers. This package adds no hash join,
automatic transient index, Bloom filter, or speculative join reordering.
Parameterized and automatic access work remains separately reviewed under
ADR-0053's correctness/scaling roadmap.

### 5. Keep remaining join completion explicit

USING/NATURAL merged columns and RIGHT/FULL output remain the next declared
join-completion package. Do not accept a partial source clause or return empty
success for any unsupported form.

## Verification

Observe focused public-session INNER/LEFT red behavior before implementation.
Cover operator/condition spans, malformed clauses, aliases, forward ON scope,
source-name ambiguity, LEFT matches/unmatched/empty sources, and WHERE
rejection without fake extension.

Pin multi-LEFT chains, mixed inner/outer sources, NULL fields/rowid/default
suppression, all row/aggregate destinations, zero/early LIMIT, reset/rebind,
actual source/spill errors and cleanup, and ownership/allocation boundaries.
Add native differential vectors and independent deterministic models/fuzz.
Do not use timing or unspecified callback order across different access plans
as a substitute for row, error, and required phase evidence.
Pin one acquired cursor and one NULL-publication site per LEFT source, one
readable result body, and linear bytecode growth through the 64-source limit.
This is the bounded correctness baseline; join scaling and automatic access
remain the separately admitted ADR-0053 performance package.

Use reviewed local commits for contracts, canonical dependency refactoring,
producer behavior, and evidence, publishing one coherent slice only after
closure. Review corrections incrementally; perform final integrated gates once.
Existing frozen workload, fixture, runner, and baseline inputs remain unchanged.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0073,
ADR-0075, and ADR-0076.
