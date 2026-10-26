# ADR-0156: Canonical Instruction-Kind Admission

- Status: Accepted
- Date: 2026-10-26
- Module graph node: `implement-advanced-sql`
- Prerequisite closure: ADR-0153 sanitizer qualification

## Context and completed mapping

The public retry of candidate `1da64b3` actually executes the original four
combined OOM partitions. Three pass in 9.88, 9.82 and 9.83 seconds; partition
three reaches the unchanged ten-second watchdog. The native differential
family has not executed when CTest stops. This is not a passing sanitizer
gate, despite successful native MSVC and complete Full quality jobs.

Existing, out-of-window phase diagnostics identify preparation as the
dominant work. A bounded, byte-identical native-executable diagnostic of
partition three takes 12.373 seconds locally: about eleven seconds precede
the aggregate step sweeps, with 11.383 seconds of user CPU. It is diagnostic
only and does not replace the original CTest result.

Owned-child stack samples reach catalog parsing, plan lowering and
`ProgramBuilder::Append`. The latter sample enters libstdc++ 13's
`std::__find_uniq_type_in_pack` through `std::holds_alternative`.
The installed header defines `holds_alternative` with a direct constexpr
function call; its utility implementation loops over the alternative pack.
Constant evaluation is not required at that call in an unoptimized build.
No compiler, language, sanitizer or optimization flag is changed.

`ProgramBuilder::Append` currently chains 27 such predicates to reject
numeric-target branch instructions. The builder already owns one typed
`Instruction` variant; discovering its alternative repeatedly is redundant.

Inventory found the canonical `InstructionKindOf` in the same module. It
reads the variant's stored index once and is already used by value/read
subroutine classification. `InstructionKindName` and module tests preserve
the established alternative-to-kind contract. No new opcode enum, visitor,
lookup table, type-index helper or container is required.

Pinned SQLite `vdbeaux.c:263-301`, `sqlite3VdbeAddOp0/1/2/3`, receives the
already classified opcode, checks its range, grows the existing operation
array only when necessary and publishes the instruction. It does not search
a type inventory for each appended instruction. Modern retains its typed
operands, explicit owned-label capability and validation boundary instead
of adopting SQLite's numeric-operand representation.

## Decision

Replace only `ProgramBuilder::Append`'s predicate chain with a switch on
the existing `InstructionKindOf(instruction)`.

Keep exactly the same 27 rejected alternatives and return the same
`kBranchRequiresLabel` error before storage reservation or publication.
Every other kind follows the unchanged `AppendPending` path.

Preserve moved-builder admission, error precedence, label ownership,
instruction addresses/order, resource and owned-byte limits, growth,
allocation boundaries, rollback and exception behavior. Do not modify
`InstructionKindOf`, the enum/variant layout, labels, verifier, VM dispatch,
parser, catalog caching, SQL or test fixtures.

This is a behavior-preserving refactor to reuse an existing canonical
classification mechanism, not a new compiler feature or a performance
exception to typed validation.

## Verification

The existing 148-case typed-bytecode selection is green before refactoring.
Start from that real baseline rather than inventing a failing behavioral
test. Retain all direct-branch rejection, label, moved-owner, limit,
malformed, analysis and allocation controls.

Mechanically compare the old rejected instruction types with the new kind
set using the canonical enum/variant mapping. Require identical branch
membership and unchanged append/error paths. Verify all four combined OOM
partitions' allocation-count and phase outputs remain byte-identical.

Run focused bytecode, allocation and OOM controls plus the complete fast
unit tier. Measure the same fixed-work OOM diagnostic before/after, without
counting debugger or extended runs as qualification. Actual final-head CI
must pass every selected case, including the original four partitions,
native differentials and unchanged fifteen-minute job envelope.
Native MSVC, production quality, GNU and all four original fuzz campaigns
remain required. Do not split sweeps, omit a prefix, change partitions,
raise a limit, disable instrumentation or substitute diagnostic evidence.
