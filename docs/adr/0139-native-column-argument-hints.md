# ADR-0139: Native Column Argument Hints

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Evidence and existing owners

PR #241 closes ADR-0138 at `d8fd021`, tree `c886da1`. Its lazy header,
requested-field acquisition and large-value ownership remain canonical.
The compiler still materializes every scalar-function argument.

Thirty-seven native/Modern valid-image comparisons agree on results across
local/nonlocal TEXT, BLOB, numeric, REAL-affinity and NULL fields. Native
file callbacks, not cache misses, establish the remaining work difference:
on the bounded large-row fixture, `typeof(b)` and `length(b)` add no native
reads over the native rowid control, but Modern adds ten page reads over
its own control. `typeof(t)` and `typeof(n)` add nineteen Modern reads.
TEXT and numeric `length` retain their actual data reads. Four native
limit-after-prepare controls allow nonlocal BLOB type/length under a
64-byte limit but reject ordinary BLOB or large TEXT-length acquisition.
Overridden native `length`/`typeof` functions read actual argument bytes.

The native callback control includes a transaction/version read absent from
Modern's page metric. Compare each engine with its own rowid control;
do not infer physical I/O from cache misses or equate absolute metrics.

| Responsibility | Pinned SQLite source |
|---|---|
| Function argument-use flags | `sqliteInt.h:2071-2073,4118-4120`, `func.c:3421-3425` |
| Resolved-definition hints on direct column arguments | `expr.c:5516-5538` |
| Local ordinary stores and nonlocal omission predicate | `vdbe.c:3248-3382` |
| Type and length consumers | `func.c:79-153` |
| Header, physical ownership and cleanup | ADR-0138 source map |

Reuse `ScalarFunction`, immutable `BoundScalarFunction` metadata, the current
source-frame/grouped/snapshot routing, typed bytecode, `ResolvedCall`,
`RecordColumnLayout`, `ReadRecordField` and canonical value stores.
Current `SqlValue` is 56 bytes and `Instruction` is 28 bytes. Its public byte
views always describe actual backed storage.

## Decision

### Translate the producer/consumer promise without invalid byte views

Native Column can publish a logical byte length larger than its placeholder
backing because only a trusted metadata consumer will inspect that value.
Do not reproduce that representation with a C++ span, add a second value
registry, or weaken ordinary `SqlValue` getters.

Represent the adjacent Column/function operation as one typed
`CallColumnScalarInstruction`, carrying function/collation symbols, cursor,
field, retained argument register, result register and REAL-affinity intent.
This is an internal opcode representation change, not another executor or
record reader. Retain the argument register separately from the result so
ordinary fallback buffers survive repeated calls. Do not bit-pack IDs or
change existing ID ranges merely to hide instruction-layout cost. Measure
the resulting canonical layout and qualify non-target memory/timing effects.

The instruction resolves the actual function with arity one through the
existing `ResolvedCall` owner. Describe trusted core argument use as
full value, storage type or BLOB length. Derive this property from the actual
core callback implementation, never its SQL name. Copy it into bound function
metadata and include it in the existing interning identity. Existing custom
constructors and callbacks retain full-value behavior.

The VM checks the resolved runtime definition again. A custom registry using
the same function name must receive ordinary bytes and its usual collation.
Keep one canonical type-name result kernel and the existing length-result
kernel; both ordinary callbacks and metadata consumption use them. Do not
duplicate scalar conversion, UTF-8 counting or function-name recognition.

### Keep native admission and the existing field path

Restore actual cursor authority before considering metadata. Share the
existing cursor-field delivery operation for rowid, virtual/deleted NULL,
missing defaults, relation-backed and physical cursor sources. Parse the
requested header once through `ReadRecordFieldLayout`.

Only a present **nonlocal** field uses the native omission predicate:
storage type can omit all field bytes; BLOB length can omit BLOB bytes.
Local fields, TEXT length and positive-size numeric length continue through
ordinary acquisition and canonical function invocation. Zero-size fields
already need no data read. Missing defaults keep their existing ordinary
delivery; this slice does not redefine their format or admission policy.

Factor serial storage-class classification from the canonical codec kernel.
Strict format/schema/reserved-code admission remains unchanged; consumed
Column metadata retains ADR-0138's native NULL/legacy rules. A nonlocal
type-only REAL uses its serial class, as native does without reading its bits;
ordinary/local NaN decoding still normalizes to NULL. Apply the existing
REAL-affinity intent before type-only consumption.

A successful metadata path clears the retained argument's logical value
through the existing store policy and publishes only the final function
result. No fake byte extent escapes into registers or public rows. Skipped
bytes do not undergo acquisition or acquisition-size checks; headers,
requested structural bounds and the actual final result still do.
Fallbacks use the same retained buffers, immutable sharing and errors as
ordinary Column. Preserve cursor/cache/value cleanup on every exit.

Lower only a unary trusted function whose direct bound column currently
comes from a live cursor. Reuse the canonical source-frame resolution rather
than duplicating it. Grouped representatives, retained snapshots, rowid
expressions, aliases/wrappers and other expressions retain their existing
materialized argument route unless that route already yields the same direct
column. Do not reload a grouped or captured value from the wrong row.

The verifier validates cursor, field, symbols, registers and positioned
authority. Model the argument and result as initialized outputs of the
combined operation, including subroutine summaries and joins. Preserve
function-call `may_abort` attribution in writable lowering.

## Delivery and qualification

Deliver one coherent implementation source after design acceptance. Begin
with a focused runtime failure for a nonlocal metadata-only request under the
bounded value limit. Keep complete fresh fast units and review before commit.

Cover native omission versus ordinary controls, local/index/relation/table
sources, REAL affinity, NULL/defaults, grouped/snapshot/captured routing,
runtime function overrides, repeated buffer retention, moved/tripped cursors,
header versus skipped-data I/O failures, OOM, reset and result lifetimes.
Reuse the current native fixtures and failure helpers; do not add large
committed data sets or relax any assertion, budget or model deadline.

Close against the actual final-head plan, preserving the original sanitizer
model, 2,000-byte negative control and 36/37 lowering budgets. Record canonical
layout, allocations, fixed work and fresh matched timing/non-target evidence.
Require GPT-5.6 Sol implementation/integrated review, a published complete
ledger and actual current hosted policy before guarded squash and cleanup.

This does not add `octet_length`, IN/null-probe compilation, new SQL, UTF-16
or another DAG node. Remaining VM attribution and ledger entries 13-15 stay
open after this bounded correction.
