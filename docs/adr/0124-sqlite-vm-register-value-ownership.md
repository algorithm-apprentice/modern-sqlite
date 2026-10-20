# ADR-0124: SQLite VM Register Value Ownership

- Status: Accepted
- Date: 2026-10-20
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12 (`execution-vm`)

## Source mapping and current evidence

ADR-0123 closes shared cursor metadata reuse, not the remaining VM gap.
The unchanged post-metadata controls retain 4.646093x covering-index,
6.388966x global-function and 4.244052x integer-sort wall ratios.
The text-projection control performs 2,150 Modern allocations.

The pinned VM separates a memory cell's logical value from reusable byte
ownership; the current `SqlValue` variant destroys that ownership when its
active alternative changes. Field instructions also create a fresh owned
value before replacing a register. Halt closes capabilities but leaves
register bytes alive until Reset or destruction.

| Responsibility | Pinned SQLite source |
|---|---|
| Logical value versus retained `zMalloc` | `vdbeInt.h:230-270`; `vdbemem.c:250-324,993-1060` |
| Ordinary Column byte copies and reuse | `vdbe.c:3247-3378` |
| Constants, parameters, copies and moves | `vdbe.c:1427-1760`; `vdbemem.c:1187-1249` |
| Ready/running phase and busy binding rejection | `vdbeapi.c:835-886,1721-1732,1854-1868` |
| Result suspension | `vdbe.c:1806-1840`; `vdbeapi.c:834-980` |
| Cursor then register cleanup | `vdbeaux.c:2190-2236,2749-2860` |
| Distinct large-overflow sharing branch | `vdbe.c:741-817`; `printf.c:1677-1727` |

Independent nine-row native/Modern controls return identical values.
Native ordinary storage retains the same 2,560-byte allocation through
TEXT, REAL, INTEGER and NULL transitions, then releases it at Halt.
Modern drops capacity on scalar transitions. A nonlocal table value over
4,000 bytes instead uses external reference-counted column-cache storage;
it is not evidence that ordinary register buffers need no ownership.

The pinned native cell measures 56 bytes; current Clang `SqlValue` measures
32. A layout-only logical/backing prototype measures 56 bytes. The four
selective lazy-value programs actually use 13, 16, 23 and 26 registers,
not a guessed larger count. Their projected register arrays remain below
the original 2,000-byte failure boundary, and register allocation occurs
during Prepare before that Step-specific failure is armed.

## Decision

### Keep one canonical value

Retain `std::vector<SqlValue>` as the register and parameter container.
Do not add a shadow register/value array, interpreter, compatibility mode
or migration representation.

Separate the private logical scalar/byte view from its byte backing inside
`SqlValue`. Use standard-defined C++23 alternatives, not inactive-union reads,
layout casts, allocator-usable-size extensions or owning raw pointers.
Public factories and Clone continue to create independently owned values;
preserve short-string factory allocation behavior.

Typed backing states distinguish external owned TEXT/BLOB storage from the
internal reusable `ByteBuffer`, mirroring external destruction versus
`zMalloc` retention. Scalar and NULL register stores release external
ownership but keep the internal buffer. Byte stores grow only when needed,
expose only logical bytes and preserve the ordinary native terminator space.
Never expose unused high-water bytes as part of a value.

Hoist the existing `SqlValueAccess` into one private runtime header rather
than duplicating access helpers or exposing borrowing through the public
owned-value API. Reuse existing downward relative-private-include patterns.
Add its actual path to the audit population when introduced.

Owned moves must rebind views after moving an inline string; moved-from
values remain valid NULLs. Clone remains a deep logical copy. Keep decoded
NaN normalization, exact numeric behavior, empty TEXT/BLOB distinction,
embedded NULs, self-assignment and byte-view aliasing well-defined.

Factor both existing high-water copies, `RecordFieldView::CopyTo(ByteBuffer&)`
and index refill's `previous_record`, into one canonical private retention
primitive. The refill path delegates with zero terminator reserve and keeps
its separate logical record length, VM limits and allocation/OOM controls;
register byte stores reserve the ordinary native terminator space. Reuse
`ByteBuffer` and `CopyBytes`; a grow must copy from a possibly aliased old
source before replacing its owner. Preserve typed allocation failures and
strong ownership guarantees. Do not leave either existing consumer on a
second buffer algorithm.

### Translate stores, copies and lifetime

Use one field-to-register helper for table/index, sorter, top-N and relation
producers. Check logical VM value limits before copying and write directly
into the destination's reusable storage instead of creating an owned
`ToOwned()` temporary. Keep missing-default and affinity handling.
Ordinary scalar stores use the same canonical logical-update primitives;
owned callback results still transfer ownership.

Internal constant and parameter loads may borrow immutable bytes because
the stable program and bindings outlive the execution. Provision these
states only through the private runtime access contract, not public
factories. Forbid rvalue borrowing. Copy instructions preserve such static
borrowing, but copy nonstatic bytes into destination-owned storage; output
aliasing must not alter inputs or leave a dangling view.

Translate the native ready/running distinction through `VmState::Running`,
not a parallel activity flag. Enter it before execution validation/dispatch
and leave it only through row, halt or error publication. The existing
ready-only Bind/ClearBindings rule then also excludes active callbacks.
Reject recursive Step, Reset and detach while running; a rejected nested
Step does not own the outer execution's cleanup, including diagnostic OOM.
This closes parameter-owner invalidation before enabling static register
borrows, without admitting concurrent or destructive reentrant VM use.
SQLite's clear-bindings implementation itself has no ready check; retain
Modern's existing stricter ready-only contract.

Bindings still clone caller values. Public Clone and callbacks cannot export
an engine-internal borrowed value as an independently owned result.
Reset/clear-binding/move/destruction order must end register borrows before
their parameter or program owners. A canonical execution cleanup closes
cursor/aggregate/temporary owners, then releases register storage at Halt,
error, reset, detach and destruction, including foreign-exception cleanup.
It does not discard parameter bindings or change completion/retry state.

Charge actual new `SqlValue` size in program-owned accounting without raising
any ceiling. STAT1's existing admission policy charges owned-value metadata;
derive that charge from the canonical backing-storage type rather than
accidentally charging the enlarged logical-plus-backing cell.
No hard-coded old ABI size or compatibility wrapper is permitted.

### Preserve the unresolved boundary

This delivery translates register ownership, not record acquisition.
Keep current complete-record validation, payload/I/O/error precedence,
mutation/NULL-row handling and snapshot/pin boundaries unchanged.
Native lazy Column metadata, projection-aware payload acquisition and the
table-only large-overflow reference-counted cache remain subsequent ledger-12
work. Do not add unused sharing/cache APIs or claim those paths complete.
No new SQL, format, planner, C API or later DAG scope is admitted.

## Ordered delivery and qualification

Use one ADR-0106 batch with three buildable source slices: canonical value
and retention primitives; field/scalar stores plus execution cleanup; then
private immutable constant/parameter borrowing and native copy semantics.
Review and commit this design before production changes, and qualify/review
each source slice before its successor.

Reuse module tests and existing global allocation/failure injectors. Add a
runtime-owned allocation runner only where no current runner covers the
new private value behavior. Cover public factory/Clone independence,
inline/heap string moves, logical versus physical length, scalar/NULL
retention, aliased grow, allocation failure, source/destination overlap,
static-load lifetimes and every cleanup path. Exercise existing caller
ABI/resource controls and the unchanged selective 2,000-byte boundary.
Include active-callback binding/lifecycle rejection and nested diagnostic OOM.

Focused red/green or a green refactor baseline and complete fast units follow
a full default development build. Run deadline-bound tests after compiler
jobs finish, with unchanged deadlines. Parent owns routine gates; GPT-5.6 Sol
reviews design and implementation.

Close the actual final-source plan once for the batch: compiler/sanitizer,
native/interoperability, original models, fuzz, allocation and fresh matched
index/read/aggregate/sorter controls. Keep timing libraries uninstrumented,
all frozen workloads/guards unchanged and failures explicit. Publish the
durable complete ledger and require integrated exact-head acceptance and all
nine hosted successes before guarded squash merge and branch cleanup.
This prefix does not complete ledger 12, node 43 or later nodes.

## Qualified register-ownership prefix

Source `43abc2d`, tree `d2380b2`, completes the three ordered source slices.
Each slice has focused red/green, a fresh default build and complete fast
units, GPT-5.6 Sol acceptance, an immutable commit and published ledger.
The final exact-source plan passes all 44 steps: 27 shared checks reconciled
over seven trees/161 instances; 58 native contracts; 1,641 isolated cases each
in Debug, Release, ASan/UBSan and GNU 16.2 Debug/Release; 1,660 instrumented
cases; and four actual 20,000-run fuzz campaigns with seed 102.
The original ten-second correlated ASan model passes in 1.14139 seconds.
The 2,000-byte selective fault and 36/37-allocation lowering contracts remain
unchanged; the final fast tier is 1,505 ordinary plus 94 isolated units.

Matched unchanged inputs, configurations and nine-sample VM controls record:

| Control | Before | After |
|---|---:|---:|
| Covering-index wall ratio | 4.646093x | 4.351259x |
| Global-functions wall ratio | 6.388966x | 5.850506x |
| Integer memory-sort wall ratio | 4.244052x | 3.886008x |
| Noncovering text projection allocations | 2,150 | 103 |
| CREATE INDEX allocations | 46,437 | 13,669 |

All fifteen index guards pass. Fixed work, nonmemory Modern counters and every
SQLite counter are identical. Not every allocation count falls: UPDATE rises
from 666 to 698 and DELETE from 626 to 642, including first retained register
buffers; their wall ratios improve from 1.077781x to 1.041926x and 1.068798x
to 1.017208x. This is bounded evidence, not engine-wide performance parity.
Local qualification does not replace final integrated acceptance or the nine
exact-head hosted successes. Native Column acquisition, large-overflow sharing
and the remaining VM attribution stay on ledger 12.
