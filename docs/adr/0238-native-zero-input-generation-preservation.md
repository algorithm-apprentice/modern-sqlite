# ADR-0238: Native Zero-Input Generation Preservation

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #312 main `f7e15cd7`, ledger312
- Related contracts: ADR-0035/0036/0106/0111/0104/0120/0234/0237

## Requirement and completed bounded inventory

The remaining deferred-generation work requires the original expression
generation sites even when a WHERE guard prevents every input row. A proof
that execution produces no rows does not remove preparation-time generation.

The pinned native false-WHERE-term bypass emits a branch to the loop's break
label, marks that term coded, and then continues normal WhereLoop construction.
SELECT still generates its source and result body. `sqlite3FinishCoding`
subsequently emits initialization and returns to the guarded main body.

Fresh native EXPLAIN observations on qualified main312 retain a covering-index
scan and result expression behind the false guard. A constant subexpression
containing `abs` through a COLLATE construction barrier is initialized before
that guard. The corresponding source-free query also retains its result body.
These are actual pinned-source observations, not a bytecode-layout requirement.

Fresh paired callback controls distinguish the broader unclosed requirement:
16 native/Modern cases have equal rows and no preparation callbacks, but nine
retain deferred-generation count differences. Reusable requests need one
callback where separate fixed requests need two. Empty input, an unreached
conditional arm, and unconsumed original VALUES producers still need their
native initialization work. Ordinary reached-once and disabled-membership
controls match. This proposal does not claim to repair those runtime gaps.

Ten additional preparation controls across native, ReadSession and WriteSession
show that the investigated malformed-literal, collation and scalar-width
errors already retain their error categories under empty input. Their Modern
diagnostic prefixes remain explicit. Do not invent a new empty-input
preparation-error regression from the callback discrepancy.

| Responsibility | Canonical source |
| --- | --- |
| Emit false-term bypass, then continue normal planning | `sqlite3.c:176553-176617` |
| Generate result expressions despite unreachable row code | Native EXPLAIN; `sqlite3.c:150695-150783` |
| Distinguish temporary/reusable and fixed requests | `sqlite3.c:118698-118906` |
| Prior-error barrier and forward deferred drain | `sqlite3.c:126820-126950` |
| Replace the current ordinary diagnostic on another error | `sqlite3.c:37060-37083` |
| Current early empty classification and discarded terms | `AnalyzeReadPredicate` |
| Current empty-only access candidate | `AnalyzeNonjoinedCoreAccess` |
| Current empty aggregate strategy and leaf publication | `BuildAggregateCorePlan`, `BuildNonjoinedAccessLeaf` |
| Existing physical guard ownership | `PhysicalGuardNode`, `PhysicalFilterNode::guards` |
| Existing source-ready access solver | `WhereLoopBuilder` |
| Existing pre-open guard and source generation | `EmitGuards`, `EmitTableScan`, index/rowid emitters |
| Current skipped result body | `PlanLowerer::EmitEmpty` |

`ReadPredicateAnalysis::empty` currently stops classification at a known
false or NULL term, discards row terms and publishes an empty-only candidate.
Aggregate and nonaggregate publication both consume that shortcut. Lowering
then emits limits, preceding guards and completion without generating the
ordinary source/result body.

The existing guard and ordinary source machinery already supplies the needed
runtime behavior. No second expression walker, shadow emitter, fabricated
cursor, compatibility plan or new runtime state is necessary.

## Decision

### Preserve false terms as ordinary pre-open guards

For the shared nonjoined read-query analysis, retain a known false or NULL
predicate instead of terminating planning. Use the existing source-dependency
and determinism rules to place it in a pre-open guard or a source-ready row
term; do not hoist an expression merely because its eventual truth is known.
Continue classifying the remaining conjuncts in their original order.

For a source-free query, every retained predicate remains an ordered guard,
including nondeterministic predicates. For a table-backed query, retain the
existing `IsStatementGuard` distinction between pre-open work and source-ready
row terms. Do not hoist nondeterministic per-row calls.

Keep the current treatment of known-true terms in this bounded source. This
does not authorize new value-semantic folding, claim a complete native
predicate-generation policy, or change ORDER BY constant classification.

Remove the now-unused read-analysis empty state and its empty-only candidate,
aggregate-strategy and leaf-publication branches. Use the same existing
WhereLoop selection, source order, key blocks, range boundaries, aggregate
choice and direction logic as an otherwise comparable guarded query.
Do not manufacture a scan only to run a second generation pass.

Keep canonical publication validation synchronized with construction. It
must validate the actual retained access and guard sequence, not accept an
empty leaf or weaken candidate/strategy equality for this case.

### Generate the ordinary body behind the guard

Reuse existing pre-open guard emission and ordinary source producers. The
false guard must branch to the existing closed/empty-input completion before
opening a source or executing its row body. The compiler nevertheless emits
the normal result, nested query, aggregate and ordering code associated with
that retained plan.

Preserve guard order: preceding applicable guards retain their current
execution and failure behavior; a later guard must not execute after an
earlier false guard. Retaining its code is not permission to evaluate it.
Keep existing LIMIT/OFFSET and aggregate completion ownership, including the
single global-aggregate result over zero input and no grouped result.

Ordinary function callbacks in the unreachable body remain unexecuted.
Existing reached-once sites, original VALUES identities, private register
ownership, query/coroutine definitions and entry construction remain governed
by their current contracts. Do not add a deferred queue or change the VM or
verifier merely to make unused code acceptable.

The public physical plan for an affected query intentionally describes its
real guarded access instead of `EMPTY RESULT`. Preserve the existing public
kind names and unrelated empty-node handling; this is not a general public
plan representation migration.

### Keep the scope explicit

This correction applies to nonjoined read-query cores through their shared
owner, including source-free, aggregate, compound and mutation-query hosts.
Joined planning keeps its existing source/guard machinery. Top-level UPDATE
and DELETE empty-access handling is a separate remaining generation owner,
not silently changed through this read-side correction.

Do not activate non-function deferred evaluation, reusable-expression search,
synthetic-zero reuse, signed-token materialization or diagnostic accumulation.
The native deferred drain is forward queue order; a later ordinary error can
replace its diagnostic. It is not a reverse drain.

Retain the nine observed deferred callback differences and the existing signed
and preparation-order differences honestly. This source restores one required
generation boundary; it does not close the general generator, IN or node43.

## Focused proof and bounded delivery

Independent design review precedes production changes. Add one owning
optimizer/lowering regression that requires the original access and generated
expression body behind a false guard, then record its actual current failure.
Pair that structural generation contract with runtime checks for zero rows,
no unreachable callback and no source-row work.

Cover false and NULL guards, source-free guard order, existing preceding
guards, the source-ready/non-deterministic distinction, table/index access,
global and grouped aggregates, and an empty arm of an existing compound.
Preserve parameter-guard and ordinary nonempty controls.
Update only directly dependent old `EMPTY RESULT` expectations.

Record optimizer and lowering allocation/retirement and program-shape
baselines before implementation. The retained planning and body can increase
preparation work; report that cost without a speed claim. Do not substitute
binder-only samples for optimizer/lowering evidence.

Reuse the existing allocation-failure owners and their unchanged limits.
Extend a current owning sweep only if the new combination lacks coverage.
Preserve all 115 native/write and 103 read preparation protocols, the delivered
original-site controls, and the fresh 16-case deferred/reached caller matrix.
The callbacks remain a baseline for subsequent generation work.

Run focused red/green, the complete fast tier, the actual selected closing
plan, strict compiler/checker owners and required hosted gates. Keep source,
failed and successful evidence distinct. Independent source, integrated and
actual-main review and normal node/ADR/ledger traceability precede delivery
closure. No later DAG node begins with this prerequisite.
