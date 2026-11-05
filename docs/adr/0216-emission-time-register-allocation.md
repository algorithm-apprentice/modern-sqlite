# ADR-0216: Checked Emission-Time Register Allocation

- Status: Accepted
- Date: 2026-11-05
- Module graph node: `implement-advanced-sql`
- Prerequisite: PR #295 main `f4262fc9`, tree `0ede51e9`, ledger235
- Related contracts: ADR-0031/0036, ADR-0106/0111, ADR-0151 and ADR-0204/0215

## Completed ownership mapping and current requirement

The remaining native generation owner needs registers for actual generated
occurrences, not one globally cached result per bound expression. Fresh
matched main235 execution preserves the eight-case constant-function
baseline: native versus Modern callback counts are1/3 for projection,
conditional and correlated-query cases,2/4 for an alias guard plus result,
and2/6 for two source occurrences. Empty, LIMIT0 and uncorrelated-query
controls match. Every row matches and neither engine invokes the callback
during preparation. These are fixed-work observations, not a behavioral red
or an optimization claim.

The pinned owner already defines the required allocation boundary:

| Responsibility | Pinned source or current implementation |
|---|---|
| An actual function-generation site enters reached-once generation | `sqlite3.c:118259-118264` |
| Allocate a result register after the VDBE already exists | `sqlite3.c:118698-118740` |
| Preserve caller-supplied versus reusable result destinations | `sqlite3.c:118698-118740,118846-118852,118886-118921` |
| Allocate argument ranges while generating a function | `sqlite3.c:118306-118345` |
| Final initialization and ready-state publication | `sqlite3.c:126820-126950`; ADR-0031's `vdbeaux.c:2658-2755` map |
| Initial expression/query/read/write resource layout | `StatementLoweringState::next_register`, `PlanLowerer::AllocateRegisters`, `FinishRegisterLayout` |
| Mutable assembly count and immutable program boundary | `ProgramBuilder::Create`, `ProgramBuilder::Build`, `BytecodeProgram::Create` |
| Runtime register ownership and complete retirement | `Vm::Impl` construction, `Reset`, `Fail`, `ClearExecution` |

Modern fixes the assembly register count at `ProgramBuilder::Create`.
`Append` owns instruction/byte/label checks; publication validates resource
ranges and definite initialization using that count. The current builder has
no checked way to append a register range after instructions exist.

Do not solve that gap by preallocating the maximum resource limit, assigning
one cached result per expression ID, adding a scratch builder, mutating a
published program, or weakening verification. Alias occurrences demonstrate
why expression identity is not a sufficient reached-once cache identity.

This source establishes only the missing register-allocation primitive.
Complete reached-once/deferred scheduling, native caller modes, preparation
ordering and signed generation remain separately unaccepted integrations.

## Decision

### Extend the existing mutable builder

Add `ProgramBuilder::AllocateRegisters(std::size_t count)`, returning the
existing typed `ProgramResult<RegisterId>`. It appends one contiguous range
after the currently declared register prefix and returns that prefix's end
as the first ID. It changes only the existing `ProgramInput::register_count`.
No new counter, arena, register container, instruction, descriptor or public
carrier is introduced.

Check builder usability first. Check growth against both the configured
`maximum_registers` and the unsigned32-bit count limit before adding or
narrowing. Use subtraction-based available-capacity checks, not overflowing
addition. A rejected request returns the existing `kRegisterLimitExceeded`
and leaves the count unchanged. A moved-from builder returns the existing
`kInvalidBuilder`.

An empty request returns the current end without publishing a register,
matching existing zero-argument range semantics. A successful allocation
does not initialize its registers. They must still be written on every
incoming execution path before an instruction reads them; a runtime default
NULL is not a verifier proof.

Keep `Create`'s existing nonzero initial-count API and validation order for
its other callers. Labels, instruction addresses, constants, parameters and
previous register IDs remain unchanged by register growth. The operation
allocates no heap storage and does not change builder/program footprint.

`Build` continues through the same canonical immutable-copy factory and
complete verifier, using the final count. Do not change the existing
publication, move or exception contracts as part of this source.

### Use the primitive at current read and DML builder creation

Replace the four identical read/INSERT/UPDATE/DELETE builder-creation blocks
with one private `PlanLowerer` helper. Keep every caller's completed
`FinishRegisterLayout` barrier before this helper. It supplies the same
schema version, parameter count and limits, creates the builder with an
empty register prefix, then allocates the already-computed initial layout
through the new primitive before publishing the builder into the existing
shared statement state.

The initial allocation starts at register0 and produces exactly the previous
resource count. Keep the existing layout order, expression homes, argument
blocks, query pairs and limits. Layout failure remains before builder
creation, including register-before-parameter limit precedence.

The layout counter describes that initial prefix; the builder owns the
mutable assembly count. This source adds no speculative post-creation
lowerer allocation branch or parallel count synchronization. A subsequent
accepted generation integration will allocate its actual occurrence ranges
through this same builder.

Schema-only builders and direct public-builder clients retain their current
initial-count calls. They do not use the shared expression-layout owner and
need no migration. No SQL evaluation order, callback count, constant pool,
cache initialization, query scheduling or VM execution behavior changes.

## Focused red, invariants and delivery

First add one owning bytecode test that emits an instruction, allocates a
later range, emits uses of both the old and new registers and successfully
publishes a verified program. Record the missing-method compile failure
before adding the implementation.

Cover configured-limit and identity-range overflow without allocating huge
runtime arrays, successful recovery after rejected growth, empty ranges and
builder moves. Keep a negative control proving that an allocated-but-unwritten
register is still rejected at publication. Retain the existing label,
resource-limit, public-input isolation and complete allocation-failure tests.

The lowerer integration is a behavior-preserving refactor over the qualified
main235 baseline. Preserve its existing read/write, query, default-stack,
typed-error and allocation controls. Reconcile the eight fixed-work cases
without claiming their five callback differences fixed.

Run focused green, the complete fast unit tier and the actual change-aware
closing plan. The public header selects full qualification; retain every
compiler, native, sanitizer, checker and bounded-fuzz obligation without
changing limits or populations. Independent design and implementation review
precede the coherent source236 commit and append-only evidence. Final exact
source/integrated/main gates and guarded source-reference retirement remain
required.

This narrowly supersedes ADR-0036's requirement that the builder cannot grow
past its initial layout. It does not relax ADR-0031/0151's immutable boundary
or verifier. The remaining name/value/scalar-width/deferred/factoring,
signed-alias and IN prerequisites remain open.
