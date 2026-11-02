# ADR-0192: Native VALUES Source Type Information

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified ADR-0190/0191 batch, PR #283
- Resumption: qualified ADR-0193 / PR #284 and PR #269 reference retirement
- Related contracts: ADR-0159, ADR-0160, ADR-0106 and ADR-0111

## Completed source mapping

The accepted preparation foundations expose the native logical selector and
function-construction provenance. They do not close the remaining behavioral
findings. This slice addresses one smaller, independently reproducible
source-metadata owner before the larger deferred-value/factoring correction.

The pinned engine rejects:

```sql
SELECT (VALUES('a' COLLATE missing_collation),('b'));
```

Modern currently prepares it and returns a row. Fourteen paired controls at
source `7787b5d5`, tree `b2615241`, expose six missing preparation failures.
They retain native-versus-Modern transcripts, source/library/binary identities
and complete-name precedence controls; they are mapping, not a TDD red.

Native `insert.c:673-789` may create a coroutine-backed chunk and a synthetic
FROM source for constant VALUES rows. `select.c:2404-2490` derives that
source's column types and collations from its first row. The name-resolution
and type-info ordering is explicit in `select.c:6497-6570`: retained
ephemeral-source type information follows names.

The source metadata is required even if EXISTS or later logical selection
omits result values. Parser-time false-AND/empty-IN deletion removes the later
source owner instead. An incoming row does not redefine the first row's
source collations. Singleton and nonconstant UNION fallback rows do not
become coroutine sources. Every eager chunk retains its own first-row owner,
including multiple chunks with a uniform eager schedule.

Executed SELECT/WHERE missing-name controls precede the source-collation
error. A discarded ORDER BY's real name-binding visit still retains the
source-type obligation. This is not a request to materialize ignored values.

The DML boundary is different. Twenty-one native preparation/scope controls
retain this error for effective, overwritten and logically pruned mutation
queries, without executing the candidate or losing prior work.
Seven additional ordering controls show that a DML child SELECT's type
error precedes later outer mutation names, but names inside that complete
child SELECT still precede its type error. `resolve.c:1909-1919` explains
the distinction: an unexpanded SELECT encountered from expression-name
resolution invokes full `sqlite3SelectPrep` at that boundary.
A blanket end-of-mutation `MoveCommon` check would therefore be wrong.

Thirty-three additional executed controls establish native diagnostic ordering.
`walker.c:131-152,206-222` visits result, WHERE, GROUP, HAVING, ORDER and LIMIT,
then FROM, and traverses compound cores from right to left. Join normalization
appends ON expressions to WHERE (`select.c:649,658`). Source columns are checked
left to right without stopping at the first missing collation; `util.c:249-272`
replaces earlier parse diagnostics. Thus the last visited missing collation
survives. Actual alias copies revisit their sources, and a fallback row's
nested sources can lie between two eager chunks of the same VALUES owner.
A first-error/FIFO/one-registration-per-constructor scheme is not equivalent.

Noncompound direct ORDER aliases and ordinals need another distinction.
`resolve.c:68-106,1748-1775` copies the native result expression before the
type-info walk. Modern's nonaggregate direct result references do not rebind
that expression. Six additional native controls verify alias/ordinal placement,
repetition, unary-positive ordinals and compound non-replay.

## Existing owners

`NativeValuesConstruction` already retains the syntax core and every native
chunk. `SyntaxNormalization::construction_binding` retains the canonical
early bound root identities before ordinary binding checkpoints.
`BindValuesQueryCore` knows which constructor cores reach name binding.
`BoundValuesCore` may compact a uniform schedule and therefore cannot
replace the original construction owner's chunk boundaries for this check.

The binder already owns collation interning/lookup, typed diagnostics,
root `RunSelect`, the `BindSubquery` mutation-query boundary, scoped modes
and temporary binding checkpoints. Reuse those mechanisms; do not add
another query tree, syntax walk, result-column model or collation registry.

## Decision

### One source-occurrence work arena

Use one binder-local flat work arena. Each node contains only indices into
the existing construction owner and one eager chunk, plus a next-work index.
Head/tail ranges borrow that arena. Do not copy queries, expressions, values,
collations or ASTs, or add another registry, query tree or query walker.

Record each actual name-bound occurrence of a synthetic coroutine source,
not one globally deduplicated node per syntax constructor. Existing query
binding identity avoids rebinding the same instance; actual template/alias
copies record their own occurrences while borrowing the same immutable
constructor roots. Record independently of value demand. Parser-deleted
queries never reach this visit. Incoming rows define no additional source;
singleton/fallback rows only contribute their nested child-query work.

Use the full construction chunks even when the public bound schedule compacts
uniform eager runs. A source node checks only its own chunk's first row;
treating all chunks as one indivisible action loses fallback-child ordering.

### Native ordering over existing binding visits

Collect ranges in scoped binder context for native clause order: result,
WHERE, appended ON, GROUP, HAVING, ORDER and LIMIT. Preserve the existing
name/error/aggregate binding order; link only the deferred metadata work in
type-info order. A successful child SELECT contributes its flattened range
to the caller's current clause. Prepend each finished compound core's range,
leaving the last core open through its ORDER/LIMIT.

Within VALUES, capture each row's nested-child work, append an eager chunk's
source node after its first row, and prepend row fragments to the preceding
VALUES fragment. Incoming cached rows introduce no child-query work.
This keeps fallback child sources between their actual neighboring chunks.

Concatenate ranges in constant time by index links, without sorting, rotating
or repeatedly scanning constructors. Work/storage is linear in actual
admitted source occurrences, with constant clause composition per query core.
Ordinary statements without multirow constructors retain an empty fast path.

### Name-root completion and cleanup

Use one binding-and-type-info finalizer for two existing boundaries:
the whole top-level SELECT, and the outermost SELECT expression entered
from a mutation root. The latter is already identified by `mutation_query`
in `BindSubquery`; finalize inside its scoped query owner before returning
to the surrounding mutation's name walk. Nested SELECTs inside either root
contribute requirements but do not finalize early. This preserves both
sets of observed name priorities and uses the same mechanism for read and
write hosts. Perform no metadata lookup or diagnostic publication before
successful completion of this name root. Constructor-owned early
value/width/parameter failures remain at their existing earlier points.

Append an arena node successfully before linking it. Query scopes retain
indices only, so arena growth cannot invalidate their links. Attach a child
range only after its complete binding succeeds. On typed or allocation failure,
restore the caller's scoped ranges and rewind the new arena suffix.
Each native name root drains and retires its work on success and failure,
before any existing mutation value-only retry.

A real discarded ORDER name visit retains its range after ordinary binding
arena cleanup because constructor roots predate that checkpoint. Actual
alias/template copies still record. Use the existing scoped-mode pattern
to suppress recording specifically for speculative compound ORDER trials,
their aggregate-bookkeeping copies, `ValidateResultSyntax` value revalidation,
and synthetic aggregate replay after a recorded discarded-order visit.
Do not suppress the original discarded ORDER name-binding visit.

Extract the existing registered-collation check from the value-demand guard
so both roles use one lookup and diagnostic kernel. The source-metadata
caller is unconditional. Drain its linked sequence in order, checking each
selected first-row column left to right through its cached bound root.
Require only a collation that root actually selects. Continue through missing
collations, retaining the last typed error as native `sqlite3ErrorMsg` does.
The ordinary value wrapper alone marks its diagnostic as `value_requirement`.
Preserve schema-lookup provenance and ordinary allocation unwinding.
Invalid owner/chunk/cached-expression/collation identities stop as explicit
invariant errors, never successful defaults.

### Direct result-reference metadata replay

Retain a borrowed metadata head/tail range alongside the existing private
`ResultDefinition` when `BindResults` publishes its actual bound expression.
Capture the newly appended part of the current result-clause range; do not
add a separate result map. Empty ranges represent expressions with no work.

At the existing direct noncompound ORDER alias/ordinal call site, retain
the aggregate template-copy route when its predicate requires it; that route
already records fresh metadata and must not replay a second copy.
Otherwise copy the definition's range into fresh nodes in the current ORDER
range, copying only constructor/chunk indices. Do not rebind values, copy query
trees, invoke functions or reuse mutable links. Do not gate this replay on
value demand or merely `has_subqueries`; the recorded range is authoritative.
Keep the bound result-reference value behavior unchanged.

Do not add this dispatch to the general result-reference overloads used by
compound ordering and other consumers. Actual WHERE/GROUP/general ORDER
template copies continue recording normally; compound/bookkeeping references
remain excluded.

A range's saved tail can acquire a later next link. Replay stops at that exact
tail, not at a null link or the current arena end. Read a node by value before
appending and retain the original arena bound; invalid indices, premature end
or cycles are typed invariant errors. Append before linking and unwind the
same scoped suffix on failure.

Ranges belong only to their active name root. Existing query scopes and
checkpoints move or retire the same definition owner. Mutation result
definitions are not exposed to later mutation roots; mutation correlation
remains unadmitted. No range is consumed after its root drains or exported as
a runtime contract.

These are binder-local preparation facts. No bytecode, VM, parser, physical
scheduling, persistent format or transaction algorithm changes are admitted,
and no public carrier grows.
Measure the private definition footprint and allocation behavior without
increasing configured limits.

## Test-first and qualification

First add one public read-session regression requiring preparation failure
for the retained first-row missing collation, and record its actual runtime
red before production changes. Then cover scalar/EXISTS/later-OR retention,
parser-deleted owners, incoming-only names, singleton/fallback rows, multiple
eager chunks, ordinary valid collations, name-before-metadata and real
discarded-order versus speculative matching. Include competing source columns,
native clause/core/chunk order, interleaved fallback children, actual alias
copies and direct alias/ordinal first/last/repeated positions. Verify compound
non-replay and allocation failure while cloning metadata nodes.

Use the shared writable facade to verify preparation failure before any
statement execution, exact retained prior writes/counters, mutation-root
versus inner-SELECT name priorities and Reset/retry where applicable.
Include overwritten and logically pruned mutation RHS controls.
Add focused ownership/allocation-failure coverage to
existing owners without raising their ceilings. Keep malformed-input,
default-stack, native/compiler/sanitizer and all existing fast controls.

Review the design before implementation and the source before its normal
commit. Run the actual change-aware and final exact-head gates, then guarded
squash, fresh actual-main qualification and delivered-reference cleanup.
This closes only retained VALUES source type information. Deferred constants,
ordinary literal/name/width/collation demand, required unary boundaries and
default extraction/generation remain separate prerequisites before IN.

The additional controls also retain native metadata-before-ordinary-scalar-
width/literal-generation observations. Modern's already recorded early value
phase gaps remain repair-required under that separate prerequisite; this
bounded source-metadata correction does not claim complete value-phase parity.
