# ADR-0218: Private Expression Register Ownership

- Status: Accepted
- Date: 2026-11-05
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #296 main `b9ed919e`, tree `1a76b4ce`, ledger240
- Related contracts: ADR-0031/0036, ADR-0082/0083, ADR-0106/0111,
  ADR-0143, ADR-0159/0192 and ADR-0216

## Confirmed failure and completed ownership mapping

The remaining generation-owner investigation finds an existing correctness
failure before implementing constant factoring. With the registered
deterministic identity probe function and the pinned catalog fixture:

```sql
SELECT (VALUES(deterministic_trace(7)),(8)
        UNION ALL SELECT items.id ORDER BY 1) AS v
FROM items WHERE v;
```

Pinned SQLite returns `2,1,3`. Modern fails during lowering with
`CoroutineRegisterConflict`; it never executes the statement. The canonical
temporary-storage factory is attached in this matched diagnostic.

A controlled replay compiles the pre-PR296 lowerer from `f4262fc9` against
consistent current headers and the unchanged remaining implementation.
The linker map authenticates both old `LowerPlan` entries and excludes the
current lowerer archive member. It reproduces the same ownership failure.
This is not a full historical rebuild or the focused TDD red.

Eight additional paired constructor/alias controls retain equal rows while
showing the still-open constant-generation differences. A native eager
constructor's generated function site is shared through actual alias copies;
ordinary generated occurrences remain distinct. Do not repair register
ownership by destroying that established semantic identity.

The completed map is:

| Responsibility | Pinned source or current implementation |
|---|---|
| Create and finish one eager constructor body | `sqlite3.c:139930-140003,139822-139829` |
| Preserve its address/register fields through real alias copies | `sqlite3.c:110495-110533,114697-114727,114774-114805` |
| Skip regeneration of an existing source body | `sqlite3.c:157552-157560` |
| Separate callable scratch from its callers | `sqlite3.c:120501-120560`; `sqlite3ClearTempRegCache` explicitly owns this boundary |
| Canonical immutable constructor roots | `SyntaxNormalization::construction_binding`; ADR-0159/0192 |
| Current global expression homes and argument blocks | `PlanLowerer::Home`, `BeginExpressionRegisterLayout` |
| Actual private instruction regions | `EmitReadSubroutine`, `EmitNativeCoroutineBodies` |
| Lexical data bindings, not instruction-region ownership | `ReadEmissionBinding`, `EmitInlineQuery`, `EmitNativeProducerBody` |
| Checked late register ranges | `ProgramBuilder::AllocateRegisters`; ADR-0216 |
| Reject shared private writes, including transitive calls | `program.cpp:4355-4445`; ADR-0143 |
| Reset/failure retirement | `Vm::Impl::Reset`, `Fail`, `ClearExecution` |

Currently `Home(id)` returns `RegisterId{id.value()}` and scalar/membership
scratch is indexed only by that same bound ID. Shared constructor roots can
therefore cause independently emitted private regions to write the same
registers. The verifier is correctly rejecting that program.

## Decision

### Separate semantic identity from physical scratch ownership

Keep the immutable bound expressions, constructor cache, source-type work,
query instances, physical plan and evaluation schedule unchanged.
Do not rebind or clone constructor roots, mutate syntax, assign VM state to
the binder, or special-case this SQL spelling.

Retain the existing main-body expression prefix and argument layout.
For emitted private bodies, key expression homes and scalar/membership
argument ranges by the actual typed instruction-region owner as well as the
bound expression ID. Reuse the existing `SubroutineId` and `CoroutineId`
identities; main is the explicit absence of a private owner.

Place this bookkeeping in the existing `StatementLoweringState`. It is
compiler register-binding state, not a SQL-value cache, another executor,
or a second mutable program owner. Immutable constants and symbols remain
statement-shared.

### Allocate only reached emission requirements

Use the existing checked builder allocator for a private home or argument
range when the corresponding emitter actually requests it. Keep one
canonical helper for scalar argument counts and membership scratch counts
across the initial and private layouts.

Reuse a binding within the same private region. A different private region
gets a fresh range even when it consumes the same bound ID. Preserve valid
zero-argument ranges. The initial layout counter continues to describe only
the pre-emission prefix; the builder remains the sole owner of the final
assembly register count.

Use one lazily allocated, statement-wide expression-indexed binding table,
not a full table per private region or a quadratic preallocation of every
expression for every producer. Entries retain their last private owner and
the optional home/argument ranges requested there. Private definitions are
emitted as complete contiguous regions, so their bindings need not be
restored after a later definition overwrites those compiler entries.
Already emitted instructions retain their concrete register IDs.

Do not add a temporary-register pool or an expression traversal to predict
requests. Preserve the existing continuation emitter; allocate at its actual
home/argument request boundaries. Main-only statements must not allocate the
private table. Register growth is linear in actual private emission
requirements and remains bounded by the existing program limits.

### Scope the real instruction owner

Enter the typed owner when emitting a registered read-subroutine body or a
coroutine body, and restore main ownership on every normal, typed-error and
allocation exit. Reject interleaved private-definition emission as an
internal invariant rather than silently losing a previous binding table.

Correlated inline queries remain in their caller's physical region.
Switching a lexical read binding or borrowing an ancestor's source context
does not switch the instruction owner. Keep existing capture, representative,
query-result/ready, producer-output and capability registers under their
current explicit owners; this change relocates expression scratch only.

Route every home and argument request through the canonical typed accessors.
Register-allocation/range/identity failures propagate through `LoweringResult`
and the existing program-error channel. Standard-library allocation retains
the existing compiler/session OOM boundary. Never return register0, fabricate an
initialized value, catch a failure as success, or publish a partial program.
An allocated home remains unwritten until its actual instruction defines it.

### Preserve verification and the unfinished generation scope

Keep the existing verifier, coroutine caller ownership, output isolation,
read-only captures, main-prologue initialization exception, cancellation
rules, resource summaries and analysis budgets unchanged.
No new VM opcode, descriptor kind, public carrier, byte buffer or runtime
register container is added. Normal VM failure/reset cleanup still owns all
register storage.

This narrowly extends ADR-0036's expression-to-register assignment for
private emission regions. It does not change binding identity, literal
materialization, name/width/collation priority, reached-once/deferred
scheduling, eager-constructor generation identity or callback counts in
already executable statements. Those remaining generation prerequisites
still need their complete accepted mechanism; this is not a global cache
per expression or a new cache per duplicated eager alias body.

## Test-first, boundedness and delivery

Add one focused owning lowering regression for the verified ordered eager
VALUES alias and record its actual `CoroutineRegisterConflict` failure before
production changes. Then cover scalar-argument and ordinary intermediate
homes, membership scratch, a singleton/nonshared control, existing nested
read/coroutine consumers, and the unchanged main-only path.

Use the existing fixture, function registry, execution helpers and owned
temporary factory. Keep the eight existing paired alias/constructor controls
and their known callback-count differences explicit; do not relabel those
differences as repaired by register isolation.

Exercise checked register exhaustion and existing complete allocation-failure
owners. Preserve all ceilings, workloads, default-stack behavior and test
deadlines. Record the private bookkeeping footprint and actual register/
allocation changes instead of hiding them behind a raised quota or claiming
an unmeasured speed improvement.

Run focused green, the complete fast-unit tier and the actual change-aware
closing plan. Review the design before implementation and the code/evidence
before its normal source241 commit and append-only ledger entry. Exact final
local/hosted gates and integrated/main qualification precede delivery closure.
Do not include the separate ordinary generation mechanism or IN admission
merely to enlarge this correction.
