# ADR-0141: Native Scalar Register Publication

- Status: Accepted
- Date: 2026-10-23
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Executed source and profile attribution

PR #243 merged as `a8f43e1`, exact qualified tree `feb54df`. Its unchanged
global-functions control is 6.285266x wall and 6.283726x CPU; operand borrowing
did not establish a timing improvement. Keep that result, not a speedup claim.

After its complete local gates, both ordinary profiles completed 1,257
iterations and 82,378,752 items with equal work, digest and completion.
Passive samples show arithmetic calling the generic owned `SetRegister`,
including TEXT/BLOB size probes, type rediscovery and scalar access after an
already computed scalar result. Value moves and backing-variant destruction
are visible too. This is one concrete dataflow difference, not attribution
of the entire remaining gap to these calls.

| Responsibility | Pinned SQLite source | Existing Modern mechanism |
|---|---|---|
| Scalar/NULL store without external cleanup when none is owned | `vdbemem.c:993-1059` | `SqlValueAccess::SetNull/SetInteger/SetReal`; `RetainedBytes` survives stores |
| Prepare the final integer destination | `vdbe.c:668-685,1427-1464` | Private scalar setters already used directly by `SetRecordField` |
| Publish arithmetic only after computing inputs | `vdbe.c:1951-2038` | ADR-0140 borrowed operands; current output is an owned temporary |
| Preserve external cleanup and retained buffers | `vdbemem.c:566-612,993-1059` | ADR-0124 canonical backing states and cleanup |

`SetNull` currently re-emplaces an already empty backing alternative.
Native scalar stores have no external destructor work in that state.
VM-owned scalar producers currently create/move an owned `SqlValue`, then
pass through byte-size and type dispatch before the same private setters.
Bytecode, register ownership, arithmetic and conversion policies need not
change to translate native destination publication.

## Decision

Use the existing canonical `SqlValueAccess` primitives and register vector.
Do not add a scalar-value wrapper, shadow array, numeric representation,
executor, parser, public borrowed-result API or alternate store algorithm.

1. In the canonical NULL transition, clear the logical value as today, retain
   `RetainedBytes`, and avoid destroying/reconstructing an already empty
   backing alternative. Release external string/BLOB/shared ownership as
   today. Keep `Release`'s complete cleanup contract unchanged.
2. Add private typed register-publication overloads for INTEGER, REAL and
   existing `SqlTruthValue`, plus an explicit NULL store. Delegate to the
   canonical setters. A primitive scalar has no byte payload, so it needs
   neither an owned temporary nor TEXT/BLOB size probes.
3. Route VM-owned primitive arithmetic, unary, bitwise, logical, comparison,
   affinity-to-REAL, rowid/root/cookie and effect outputs through those stores.
   The private logical helper returns the existing truth domain rather than
   an owned result. Keep its truth table unchanged.
4. Keep generic owned publication for actual owned function/aggregate,
   affinity/CAST, encoded-record and byte results, with their existing limits
   and ownership transfer. Generic scalar publication delegates to the same
   typed stores; direct record decoding retains the same canonical setters.

Compute every operand-dependent value before changing its destination.
Output may alias either input. Preserve checked overflow, min/-1 division and
remainder, zero division, NULL, NaN normalization, signed zero, conversion,
byte limits and error phases. No operand or scratch is read after a store
that may replace it. Retained capacity, static borrows and external owners
keep ADR-0124's lifetime rules.

## Ordered qualification and delivery

Use two sequential ADR-0106 source slices: canonical backing transition,
then VM destination publication. Each starts from a genuine green refactor
baseline, gets focused/module ownership and alias coverage, a fresh full
development build and complete fast units, GPT-5.6 Sol review, a normal commit
and append-only ledger before its successor. No artificial red is needed
for behavior-preserving refactoring.

Reuse existing tests rather than adding another corpus or harness. Cover
ownerless/retained/external/shared/static states, repeated scalar stores,
NULL/NaN/signed zero, both output-alias directions, overflow and existing
OOM/lifecycle controls. Measure the unchanged target and fixed work; retain
flat results and non-target evidence. Do not promise a timing improvement.

Close one actual-change final-head plan for the related batch, reconcile
shared checks, retain isolated compilers/sanitizers, original models,
native/generated-input and matched contracts, and publish the complete
ledger before integrated review. Keep all layouts, 2,000-byte negative
control, 36/37 allocation budgets, guards and deadlines unchanged.
Require actual hosted gates, guarded squash, exact retained-tree proof and
named cleanup. Remaining VM classification, ledger 13-15 and later SQL/DAG
work are not completed by this design.
