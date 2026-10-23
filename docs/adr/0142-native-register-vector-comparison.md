# ADR-0142: Native Register-Vector Comparison

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Completed source mapping and baseline

PR #244 merged exact qualified tree `d3d62f2` as `0dd07e7`.
`CompareRecordsInstruction` still encodes both resident register spans,
parses both records and builds comparison columns on every invocation.
Its lowering consumers are ordered compound merge and duplicate-suppression
paths, not the global-functions or grouping controls.

| Responsibility | Pinned SQLite source | Modern mapping |
|---|---|---|
| Ordered compound duplicate/merge comparisons | `select.c:3204-3214,3777-3787` | Existing ordered-compound lowering and `CompareRecordsInstruction` |
| Read resident registers in key order, then publish comparison | `vdbe.c:2528-2610` | Current VM register spans and integer result |
| NULL/storage/numeric/collated/byte ordering | `vdbeaux.c:4551-4647` | Existing templated `CompareField` in the record codec |
| Prepare immutable comparison metadata once | `select.c:1586-1626,1666-1674` | Existing `IndexColumnOrder` columns after actual environment resolution |
| Release program-owned key metadata | `vdbeaux.c:1394-1396` | Bound VM metadata survives Reset and ends with the VM |

The unchanged nine-sample controls at this baseline are:

| Existing workload | Modern median | Wall ratio | Modern allocations |
|---|---:|---:|---:|
| `ordered-union-merge-memory` | 32.718 ms | 5.850185x | 286,964 |
| `ordered-except-merge-memory` | 78.711 ms | 2.133787x | 207,093 |

These are whole-query counts, not attribution of every allocation to this
instruction. Source mapping identifies two owned record encodings and one
column-vector construction per comparison. Native `OP_Compare` does none.
Keep original SQL, fixture, configuration, input sizes and guards.

## Decision

Add a `CompareRecordPrefixes` overload accepting two borrowed
`span<const SqlValue>` values and existing `IndexColumnOrder` metadata.
Reuse the canonical templated `CompareField`, numeric helpers, collation,
direction and effective NULL placement. Do not encode resident values,
construct owned values, or duplicate field-ordering semantics.

Validate metadata through the existing validator. A resident span shorter
than the requested prefix is caller misuse, not malformed persistent data.
Preserve the existing empty-prefix equivalence and ignore suffix values.
Cursor-backed encoded overloads retain their existing strict validation and
error precedence; no fake encoded view, new decoder, callback framework or
comparison-value representation is introduced.

At VM initialization, resolve actual registered collations and move the
existing runtime conversion's `vector<IndexColumnOrder>` columns into one
prepared set per comparison descriptor. Reuse that canonical column owner,
not a new metadata class or per-instruction container. Keep current VM widths:
the physical-index `IndexComparisonInfo` has a 16-bit native shape and is not
an appropriate new limit for existing 32-bit register descriptors.

The bound columns borrow the same environment collations as existing runtime
descriptors. Retain them across Reset/detach/Halt; release them with the VM.
Contain preparation allocation failure at the existing typed factory.
Do not allocate during ordinary built-in-collation comparison.

Execute the existing instruction against resident register spans and prepared
columns, then publish -1/0/1 through the canonical integer store. Output may
alias an input: no operand is read after publication. Preserve existing
descriptor order, collation, NULL placement and lowering's register layout;
do not add permutation or SQL scope. Affinity, persistent formats, record
validation, public value ownership and all limits remain unchanged.

## Sequential evidence and delivery

Use two ordered buildable ADR-0106 sources: shared resident-value comparison,
then canonical VM metadata/consumer integration. Each requires focused red
evidence, green/module coverage, fresh full development build and complete
fast units, GPT-5.6 Sol acceptance, normal commit and append-only ledger before
its successor. A missing-overload compile failure is the first API red;
the VM source must independently observe the old per-step allocation failure.

Reuse format/VM tests and existing allocation/OOM runners. Cover numeric
boundaries, NULL/direction combinations, BINARY/NOCASE/RTRIM, TEXT/BLOB and
embedded NUL, invalid metadata, short spans, ignored suffixes, aliases,
zero-allocation repeated Step/Reset and every preparation allocation failure.
Do not add another harness or expand a corpus.

Measure both original controls and fixed work; report expected allocation
changes separately from unchanged rows, VM work, I/O and native diagnostics.
Close one final exact-head actual-change plan with existing isolated,
native/generated-input, model, quality, non-target and actual-hosted gates.
Keep layouts, 2,000-byte negative control, 36/37 allocation budgets and all
guards/deadlines. Publish the complete ledger before integrated review, then
guarded squash and named cleanup. This does not complete VM classification,
ledger 13-15 or later SQL/DAG nodes.
