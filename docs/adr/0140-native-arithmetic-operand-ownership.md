# ADR-0140: Native Arithmetic Operand Ownership

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Executed attribution

PR #242 closes the bounded Column argument hints at `e4041c2`, tree `d2ca4da`.
The original global-functions workload still has a 6.264641x wall ratio and
6.264381x CPU ratio in nine unchanged samples per engine. Diagnostic work is
1,376,287 Modern instructions versus 1,245,204 native steps, with the same
393,216 aggregate steps and no page-cache misses. These metrics have different
opcode representations; their modest volume difference alone does not
explain the timing gap.

Existing ordinary `profile` mode executes identical work for 1,257 iterations
in both engines. Passive sampling of those exact owned process IDs attributes
visible Modern work to arithmetic coercion, value cloning/moving, Column
decoding and aggregate kernels. Timing runs precede profiling; no production
instrumentation, workload or threshold was changed.

The current VM unconditionally calls owned `CoerceNumericForArithmetic` for
both binary inputs and unary negation. That public helper correctly clones an
already numeric value to satisfy its owned-result API, but a register consumer
does not need independent ownership. The global workload has three arithmetic
expressions per row, making six such numeric operand clones. Aggregate
numeric conversion has its own copies; that separate function contract stays
on ledger 13 rather than being changed here.

| Responsibility | Pinned SQLite source |
|---|---|
| Borrow register operands; integer fast path | `vdbe.c:1951-1990` |
| NULL before conversion, numeric/REAL fallback | `vdbe.c:1991-2038` |
| Numeric flags versus byte conversion | `vdbe.c:469-512` |
| Nonliteral unary minus through arithmetic | `expr.c:5372-5393` |

The canonical Modern conversion policies already distinguish arithmetic,
affinity, CAST and function numeric typing. Reuse them. Existing comparison
execution already borrows input registers and uses optional caller scratch
only when conversion is required.

## Decision

### Borrow existing numeric register values

Add one private `SqlValueAccess` arithmetic-operand access operation. It
returns a reference borrowing an existing NULL, INTEGER or REAL input.
Only TEXT/BLOB inputs use caller-owned optional `SqlValue` scratch, filled
through the existing `CoerceNumericForArithmetic` parser.

The reference borrows either its input or that explicit scratch; both must
remain alive and unchanged through the calculation. Forbid rvalue inputs.
Do not add a new numeric value representation, shadow register array,
conversion parser, cache, owning pointer or interpreter.

Use the same operation in unary negation and binary arithmetic. Binary
arithmetic checks already evaluated NULL operands before numeric conversion,
matching native operation ordering. This does not skip SQL expression or
callback evaluation that produced those operands.

Keep public owned conversion/Clone contracts unchanged. Arithmetic continues
to use existing checked integer operations, REAL promotion, divide-by-zero
and remainder handling, numeric conversion and NaN normalization. This is an
operand ownership/ordering correction, not a replacement arithmetic algorithm
or a change to bitwise, affinity, CAST, comparison or aggregate semantics.

Compute the result before storing it. Output may alias either input, so no
borrowed input or converted scratch is consulted after the canonical store
replaces a register. Retained byte storage, static program/parameter borrows,
typed errors and execution cleanup remain under their current owners.

### Qualify the measured change without broad optimization

Start from the exact green `d2ca4da` code baseline qualified for PR #242:
complete fast units, all six isolated configurations, native/fuzz/model and
quality gates. The merge commit has the identical tree. A behavior-preserving
refactor does not need an artificial failing test.

Add focused private-access tests for borrowed identity, conversion scratch,
input immutability and full conversion semantics; exercise VM NULL, numeric
boundaries, TEXT/BLOB conversion and both output-alias directions.
Keep module-owned malformed, OOM and lifecycle coverage.

Measure the same global workload and native profile/fixed-work controls after
implementation, and retain non-target matched evidence. Preserve current
value/instruction layouts, allocations, all resource ceilings, the original
10-second model and 36/37 lowering contracts. Do not claim whole-engine parity
or attribute aggregate-kernel costs to this correction.

Use one coherent implementation source after design acceptance, complete
fresh fast units, GPT-5.6 Sol implementation review and a normal commit.
Close the actual final-head verification plan, publish its complete ledger
before integrated review, require actual hosted policy, then guarded squash
and named cleanup. Ledger 12 still needs final attribution/classification;
later ledger entries and SQL/DAG nodes do not start here.
