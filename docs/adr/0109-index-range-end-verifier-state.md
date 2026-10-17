# ADR-0109: Index Range-End Verifier State

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

The direct-bytecode audit found one verifier/runtime mismatch involving
virtual NULL rows and index range checks.

This program currently verifies:

```text
LoadConstant range-key
OpenReadCursor index
NullRow index
CheckIndexRange index, end=read
read:
ReadField index
```

`NullRowInstruction` clears the physical B-tree position and publishes one
logical NULL row. `ReadFieldInstruction` can read that logical row and return
NULL. `CheckIndexRangeInstruction`, however, consumes the logical row on its
end edge: the VM clears the record cache and NULL-row publication, then jumps
to `end_target`. The underlying B-tree cursor remains unpositioned because
`NullRowInstruction` already cleared it. The following field read therefore
fails at runtime even though verification accepted it.

The verifier currently has three cursor states:

- closed;
- open and unpositioned; and
- positioned/readable.

`NullRowInstruction` intentionally uses positioned/readable because ordinary
field and rowid reads must accept the published NULL row. The verifier then
treats both outgoing `CheckIndexRangeInstruction` edges as positioned.

The same end edge is also logical exhaustion for an ordinary physical range.
The VM may retain an internal B-tree position at the first out-of-range key,
but that key is not a candidate row and no verified program should read it
through the range-end path. Current SQL lowering already binds the range-end
label to cleanup, cursor advancement completion, or cursor close; it does not
consume a field from that edge.

Existing reusable mechanisms are sufficient:

- the three-state cursor verifier representation;
- distinct fallthrough and branch-state publication used by rewind, seek,
  next, and previous instructions;
- typed `CheckIndexRangeInstruction` end targets;
- `NullRowInstruction` enable/disable cleanup; and
- the existing direct bytecode, VM, lowering, OOM, allocation, sanitizer, and
  model tests.

This is an internal bytecode contract correction. It does not change SQL
planning, B-tree comparison, index range boundaries, NULL publication itself,
or persistent data.

## Decision

### 1. Publish logical exhaustion on the range-end edge

Keep `CursorState` unchanged.

For `CheckIndexRangeInstruction`, verification performs these transitions:

1. require the cursor to be positioned/readable;
2. require every range-key register to be initialized;
3. publish the unchanged positioned state to fallthrough; and
4. publish the open/unpositioned state to `end_target`.

The ordering is significant. Fallthrough is the accepted current candidate
and remains readable. The end edge is logical range exhaustion and is not
readable, whether it was reached from a physical out-of-range key or a
consumed virtual NULL row.

This mirrors the existing verifier pattern for cursor operations with one
continuing row edge and one exhausted edge. It also allows range-end paths to
merge with other unpositioned exhaustion paths.

### 2. Do not add a distinct NULL-row cursor state

`NullRowInstruction` continues to publish positioned/readable state while its
logical row is live. This preserves ordinary NULL field/rowid reads,
Next/Previous consumption, join publication, reset, and subroutine behavior.

The range instruction itself owns the transition that consumes readability.
A fourth cursor state or a positioned-or-NULL union would enlarge every state
merge and instruction precondition without changing any other valid program.

### 3. Keep completion labels state-compatible

Lowering must not merge a range-end unpositioned edge with a positioned LIMIT
completion edge.

For covering index scans:

- seek/rewind miss, natural Next/Previous exhaustion, and range end use the
  existing unpositioned completion label; and
- LIMIT completion alone uses the positioned completion label.

For noncovering index scans:

- the first range check keeps its existing first-candidate end label because
  both index and table cursors are unpositioned;
- a subsequent range end uses the existing index-exhausted label, matching
  Next/Previous exhaustion with an unpositioned index cursor and the previous
  table candidate still positioned; and
- LIMIT completion keeps the positioned completion label with both cursors
  positioned.

Joined index range lowering already has a distinct range-end cleanup label
that disables NULL publication before joining its ordinary done path. Keep
that structure.

Do not add duplicate completion bodies. Reuse the current state-compatible
cleanup labels and the existing `EmitReadCompletion` ownership.

### 4. Do not change VM range comparison or cursor storage

The VM already clears logical NULL publication before taking the virtual
range-end edge. No VM change is required.

For a physical out-of-range key, the typed verifier deliberately models the
edge as logically unpositioned even if the private B-tree cursor still has an
internal position. That retained position is an implementation detail used
only until existing cleanup or close instructions run. The fallthrough
candidate path remains unchanged.

### 5. Preserve bytecode ownership

Do not add a second range instruction or cleanup opcode. Lowering changes only
which existing completion label owns each range-end edge.

`NullRowInstruction{.enabled = false}`, cursor close, halt cleanup, and other
operations that require only an open cursor remain valid on the unpositioned
end edge. A field read, rowid read, Next, Previous, or another range check
requires a new positioning or NULL-row publication first.

### 6. Use focused direct-bytecode evidence

Before production changes, add a focused verifier test that:

- proves `NullRow -> ReadField` remains valid;
- proves `NullRow -> CheckIndexRange(end) -> ReadField` is rejected with
  `kCursorNotPositioned`;
- proves a physical range fallthrough remains readable;
- proves the range-end edge can close or disable NULL publication; and
- proves an end edge can merge with another unpositioned exhaustion path; and
- preserves covering and noncovering bounded ranges with LIMIT/OFFSET plus
  joined index range cleanup.

Record the expected failure while the verifier still publishes positioned
state to `end_target`. Then change the verifier transition and the
state-compatible lowering targets, and run the focused test, complete fast
unit tier, isolated Debug/Release/sanitizer/instrumentation configurations,
GCC 16 Debug/Release, the selected fuzz domains, and existing native
performance contracts.

Obtain GPT-5.6 Sol design review before production and implementation/exact-tree
review before commit. This prerequisite does not complete ADR-0092 follow-up
work, outer aggregate ownership, node 43, or later nodes.

The initial review found that covering and noncovering range ends shared
positioned LIMIT completion labels. The revised state-compatible label routing
passed GPT-5.6 Sol follow-up review before production began.

## Consequences

Positive:

- verified range-end paths cannot read a consumed virtual NULL row or an
  out-of-range physical key;
- fallthrough candidates remain readable;
- range exhaustion can merge with other unpositioned paths; and
- no runtime, cursor, or persistent-format mechanism is added.

Negative:

- direct bytecode that intentionally read the first out-of-range physical key
  is rejected, because that key is outside the typed range contract;
- the verifier transition becomes asymmetric; and
- covering and noncovering lowering must keep positioned LIMIT completion
  separate from unpositioned range exhaustion.

## Rejected alternatives

### Add a virtual-NULL cursor state

Rejected because ordinary NULL rows are readable and already share the
positioned precondition with physical rows. Only the range-end edge consumes
that readability.

### Clear the physical cursor in the VM on every range end

Rejected because the defect is verifier acceptance, not B-tree comparison or
cursor cleanup. Existing verified lowering never reads the end key.

### Special-case only known NULL-row programs

Rejected because the verifier has no path-history flag and the logical
range-end contract is the same for physical and virtual rows.

### Leave the end edge positioned and add a runtime fallback

Rejected because success-shaped NULL or skipped reads would hide an invalid
bytecode state instead of rejecting it before execution.

## Traceability

Node: `implement-advanced-sql`, prerequisite closure ordinal 4.

Contracts: ADR-0053, ADR-0063, ADR-0077, ADR-0079, ADR-0086, ADR-0089,
ADR-0102, and ADR-0106.

Relevant implementation:

- `src/bytecode/program.cpp` — cursor-state dataflow and
  `CheckIndexRangeInstruction`;
- `src/vm/vm.cpp` — NULL-row publication and range-end execution; and
- `src/lowering/plan_lowering.cpp` — existing range-end cleanup labels.
