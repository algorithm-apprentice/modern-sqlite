# ADR-0221: Out-of-Line Entry Constant Construction

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #299 main `18736e19`, ledger252
- Related contracts: ADR-0031/0036, ADR-0106/0111, ADR-0143 and ADR-0216/0218/0219

## Completed placement and ownership mapping

The remaining generation owner needs storage for actual emitted occurrences,
not speculative register preallocation or one value cache per expression.
ADR-0216 provides checked late register growth and ADR-0219 provides typed
invocation-once control. Neither operation initializes a SQL value.

The existing private-construction proof recognizes only consecutive constant
loads at physical instruction0. It cannot authorize the same construction
after a main-entry jump, even when that block executes before every private
call or coroutine activation. That placement prevents constructing a private
once-result after its emission-time register identity becomes known.

Pinned SQLite instead appends initialization after generated bodies and
patches its initial control to reach that block, then returns to address1.
This source translates that placement through existing jumps and labels,
while retaining Modern's accepted explicit constant construction. It does
not implement SQLite's complete deferred-expression queue.

| Responsibility | Pinned or current source |
| --- | --- |
| Allocate actual once-result destinations during generation | `sqlite3.c:118698-118740`; ADR-0216 |
| Append initialization and return to executable address1 | `sqlite3.c:126870-126950` |
| Prior-error barrier before deferred work | `sqlite3.c:126820-126833`; remains separate |
| Invocation epoch and first-dispatch boundary | ADR-0219; existing `Vm::Impl::Step` |
| Lexical ownership and canonical flow transfer | `AnalyzeSubroutineGraph`, `VerifyProgram` |
| Current construction range and charged bitmap | `program.cpp:2815-2867` |
| Reentry and construction-write classification | `program.cpp:2873-2881,3078-3086` |
| Private/output ownership checks | `program.cpp:4370-4457` |
| Existing owning positive/negative fixtures | `PrivateCacheCoroutineProgramInput`, `program_test.cpp:781-889` |
| Shared query-result NULL construction | `StatementLoweringState::subqueries`, `EmitSubqueryCacheInitialization` |
| Read and mutation publication owners | `RunRead`, `RunInsert`, `RunDelete`, `RunUpdate` |

The current query cache is a real migration consumer. Its result bindings,
NULL values, private bodies and once sites already exist. Moving their entry
construction requires no new result cache, query identity or runtime owner.
The source249 baseline retains81 preparation/value callers and11 signed-alias
controls. All11 native tracing-derivative outcomes match the unmodified
oracle; their151 events demonstrate real deferred-error barriers. This
placement prerequisite does not claim those remaining differences repaired.

## Decision

### Recognize one logical entry construction block

Keep the existing inline main-entry constant prefix and its behavior.
Additionally recognize this bounded out-of-line encoding:

1. Physical instruction0 is a main-owned unconditional jump to a later
   main-owned address greater than1.
2. Its target begins a nonempty consecutive range of
   `LoadConstantInstruction` operations, entirely owned by main.
3. That range ends immediately before a main-owned unconditional jump to
   main instruction1. Instruction1 is outside the construction range.

Use one canonical private range description for both encodings and reuse the
existing construction bitmap, definite-initialization transfer, read/write
summaries and ownership checks. Do not add a public descriptor, instruction,
program field, execution state, register container or parallel verifier.
An unrecognized shape receives no private-construction exception; otherwise
legal ordinary control still follows the existing verifier.

The out-of-line block must execute only through its actual initial edge.
Permit instruction0's designated entry jump, not every jump to the same
address. Detect all other entries into its constant range, including
fallthrough from outside that range, and every edge back to instruction0.
Use the existing rejection of reentrant protected construction. Ordinary
fallthrough between adjacent constants remains valid.

Apply the same range predicate when collecting constructed registers and
excluding construction writes from main's ordinary write summary. Keep all
private-region control checks, coroutine caller ownership, output isolation,
sibling rejection and read-only capture rules. Constant construction still
does not authorize an outside private read or another private writer.
Published output registers remain ineligible.

Parameter, arithmetic and function instructions are not entry constructors
under this contract. Do not infer safety from a register's default runtime
NULL, from its eventual value, or from the presence of a once gate.
Both once successors remain conservative.

Preserve inline accounting and the existing numerical limits. Charge
additional out-of-line discovery/control visits, including unsuccessful
candidate inspection, to the existing analysis-work budget. Charge the same
construction bitmap and ordinary transfer state to the existing memory
budget. Use checked boundaries and existing typed errors; do not allocate a
maximum-sized region table or silently grant an exception after a limit error.

### Migrate the current shared query construction

Use one small pair of typed labels and shared private begin/finish helpers
around the existing read/INSERT/DELETE/UPDATE emission sequence. Do not add
another assembler, callback executor or instruction-address relocation pass.

When query bindings exist, ensure the existing shared NULL constant at its
current preparation point. This retains constant-pool ordering and the
existing constant-admission error boundary. Emit the initial jump and bind
the main-body continuation before its existing emission.

After the main and all delayed private bodies have been emitted, bind the
construction label, emit the same query-result NULL loads in their current
order, and jump to the main-body continuation. All instructions and fixups
use the existing checked builder APIs. Typed failures propagate through the
current lowering/program channel; no partial program is published.

Statements without query bindings create no new labels or instructions.
Retain every query binding, result register, body initialization, once site,
capture and current emission order within the main/private bodies. Do not
eliminate unused bindings or classify all main constant writes as protected
construction. Preserve the ordinary owners of other read/write state.

The VM continues advancing its epoch at the existing Ready boundary and
executing ordinary jumps and constant loads. Row suspension, coroutine
reinitialization/cancellation, failure, reset/rebind, detach and complete
register cleanup remain unchanged.

### Bound the supersession

Generalize only the physical-prefix restriction of ADR-0143's entry
construction proof. Its execution-before-activation requirement, constant-only
write policy and all private ownership/lifetime restrictions remain.
ADR-0218's physical scratch owners and ADR-0219's conservative once semantics
are not relaxed.

This does not activate ordinary constant functions, signed-alias diagnostics,
general deferred initialization, generation-error accumulation or shared eager
constructor/coroutine ownership. No query cache per expression, constructor
cloning, speculative expression prewalk, new SQL admission or IN is added.

## Focused proof and bounded delivery

Start with one owning bytecode regression that routes the existing private
cache fixture through the out-of-line entry block. Record the actual
pre-change verification failure before production edits. Then cover valid
construction and VM results/reset, while retaining the existing inline
controls.

Reject bypass/reentry, outside fallthrough, nonconstant construction, wrong
return/region ownership, sibling access and published-output construction.
Use fixtures that isolate the construction proof rather than failing first
for an unrelated uninitialized coroutine or invalid address. Keep existing
analysis and exact-allocation boundaries and add a focused charged-work
boundary for the newly inspected shape.

Treat the existing SQL query-cache layout migration as a green refactor over
qualified main252. Reuse scalar/EXISTS, captured and uncaptured queries,
read/write hosts, eager/ordered constructors, TEXT/BLOB ownership, failure,
reset/rebind and complete allocation/OOM owners. Preserve all retained
preparation and callback outcomes; this is not a generation-order repair.

Record actual instruction/control and allocation deltas. Keep query-free
work unchanged, and make no speed claim. Run focused green, complete fast
units and the actual change-aware closing plan with all original workloads,
limits and deadlines.

Independent design acceptance precedes implementation. Independent source
review and a normal coherent source253 commit/ledger entry precede a later
slice. Exact-head local/hosted/integrated/main gates and complete raw-archive
capsule authorization still precede delivery closure. General generation,
IN, node43 and the project remain incomplete.
