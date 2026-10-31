# ADR-0187: Failure-Atomic VM Execution Attachment

- Status: Accepted
- Date: 2026-10-31
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `a059fd4`, ADR-0124 and ADR-0184
- Related contracts: ADR-0101, ADR-0102, ADR-0106 and ADR-0185

## Completed current-owner inventory

PR #279 closes current qualification and both delivered-reference retirements
on main `a059fd4`, tree `0d51b28`, ledger147. Its diagnostic capability does
not prove a permanent hosted timing fix. Unmerged #269 remains preserved.

Four unexpired admission differences remain: a temporary coordinator Read
allocation runs before actual compiler-selected execution admission, losing
prior journaled explicit/named work on OOM. The larger native admission,
old guard/halt, cleanup and runtime50-reprepare correction stays separate.
Before that path exposes the old VM's actual guard, attachment must obey its
existing typed-error and borrowing contract.

The current VM has one canonical schema validator. Attachment publishes the
borrowed database, Pager, generation, writer and temporary-storage pointers
before calling it. Ordinary schema failure clears those pointers. Diagnostic
string allocation failure instead escapes the public attachment operation,
which has no OOM boundary, before that clearing code runs.

A focused unchanged-production probe through the existing VM OOM allocator
confirms both effects:

- ordinary wrong-generation attachment returns SCHEMA17 and remains detached;
- with one diagnostic allocation failure, `std::bad_alloc` escapes and the
  READY VM retains the invalid attached context.

This is not a file-I/O or native heap-ordinal comparison. The probe resets
the VM and closes its borrowed read after observing the failure.

### Canonical owners and native boundary

| Owner | Existing responsibility |
|---|---|
| `VmExecutionContext` | Borrowed capabilities; they outlive attached execution. |
| `Vm::Impl::AttachExecutionContext` | Detached READY and capability validation, schema check, context publication. |
| `Vm::Impl::ValidateSchema` | Snapshot, generation, cookie, format and encoding checks; publish record format only after successful checks. |
| `Vm::Step` | The same validator at first execution, then existing suspension/data-version checks. |
| `Vm::Reset` | Retire execution/register borrows, retain parameter ownership, return detached READY. |
| Existing read/write facade OOM helpers | Reset VM borrows and reconcile only their actual read/statement ownership. |

Pinned `vdbe.c:4245-4350` performs real B-tree admission before the old
cookie/generation guard. `vdbeaux.c:3335-3488` halts and applies error policy
before outer preparation; `vdbeapi.c:980-1027` then owns bounded reprepare.
Those mechanisms require explicit failure and ended old borrows, not an
invalid retained attachment. The completed native schema-cleanup map
separately preserves guard change-publication suppression and primary-SCHEMA
versus cleanup-OOM precedence. This ADR does not redesign those policies.

## Decision

### 1. Reuse the same validator over explicit borrowed inputs

Refactor the private validator to receive the existing borrowed Pager and
catalog generation explicitly. Attachment and first Step call that same
implementation. Preserve every check, order, diagnostic, normalization,
record-format update and snapshot-free behavior.

This is a green-baseline refactor, not a second schema predicate, validator,
cache, token, generation, execution state or public capability.

### 2. Validate before publishing the attachment

After the unchanged detached READY and capability preconditions, validate
the candidate context through that canonical function before assigning the
borrowed context fields. All fallible schema/diagnostic work precedes
publication. Valid attachment retains the same ownership and performs no
new allocations or I/O.

An expected validation error or allocation exception leaves the VM detached
and READY, with program, parameters, counters and prior execution ownership
unchanged. An invalid attempt on an already attached, suspended or invalid
VM never clears or replaces somebody else's existing context.

Add the existing-style public `std::bad_alloc` boundary returning
`Error::OutOfMemory`. There is no partial attachment to reset or silently
clean up at that boundary. Do not catch unrelated exceptions or invent a
successful fallback.

The read and write facade attachment consumers route a returned OOM through
their existing canonical OOM helpers, just as they previously handled that
exception. Preserve their current reset, read release, active-statement
rollback, primary/cleanup error, binding and counter behavior. Do not
accidentally substitute the generic execution-error path merely because
attachment now returns a typed error.

This does not introduce a schema-origin witness or change normal SCHEMA
counter publication, suspended-error classification, actual admission,
compiler journal policy, old cleanup, retry bounds or persistent state.
Those belong to the completed next runtime-owner map and its accepted design.

## Two ordered sources

1. Canonical borrowed-input validator. Start from the current qualified green
   baseline, change only private validator parameters and both existing
   consumers, then focused schema/lifecycle and complete fast units.
   Independent SourceAccepted and one normal ledger commit precede Source2.
2. Attachment publication and typed OOM. First add an owning runtime
   regression through the existing VM OOM allocator that requires typed7,
   detached READY and retained bindings after diagnostic allocation failure.
   Cover generation/cookie mismatch, valid retry, already-attached diagnostic
   failure preserving the original context, moved/suspended misuse controls
   and real row execution. Then publish only after validation, contain OOM
   and keep both facade OOM call sites consistent.

Retain existing read/write preparation/admission, schema/binding, allocation,
cleanup, malformed-header, callback/reentrancy and failure-prefix controls.
Do not defer owning tests or weaken existing assertions, allocation limits,
deadline, process stack or workloads.

Each source needs accepted design, focused green baseline or actual red/green,
complete fast units, applicable sanitizer/ownership/lifecycle evidence,
strict quality, independent SourceAccepted, normal commit and append-only
ledger before its successor. Publish one ADR-0106 draft after the first
accepted prefix; close final exact-head compiler/sanitizer/native/model/
quality/bounded campaign union, integrated review, normal squash, fresh
actual-main qualification and delivered-reference retirement.

The four unexpired admission differences and node43 remain incomplete.
No speculative phase API, new SQL feature or later DAG node starts here.
