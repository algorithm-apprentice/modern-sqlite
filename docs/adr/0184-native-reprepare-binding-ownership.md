# ADR-0184: Native Reprepare Binding Ownership

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `f173db6`, ADR-0124 and ADR-0150/0151
- Related contracts: ADR-0102, ADR-0106, ADR-0111, ADR-0182

## Completed inventory and bounded prerequisite

PR #276 is normally squashed and qualified on actual main `f173db6`,
tree `8feb3a0`, closure ledger 130. Its delivered references are retired;
unmerged #269 remains preserved. Node 43 is not complete.

The resumed native statement-admission inventory identifies two different
schema paths: an unexpired program's actual transaction/guard/halt, and a
READY-expired program's preparation before old execution. Matched public
controls confirm six remaining resource-scope mismatches in the writable
preflight owner, not an error in PR #276's default-ABORT metadata.
Those admission/cleanup/retry corrections remain separate work.

Before integrating native schema reprepare, its parameter handoff needs one
canonical ownership operation. Current read and write `Recompile` both
iterate a borrowed binding span and call `Vm::Bind` for each replacement.
That public caller-binding operation correctly clones bytes, but native
reprepare transfers already-owned bindings instead of copying them.

The completed native chain is:

| Mechanism | Pinned SQLite source |
|---|---|
| Prepare replacement before publication | `prepare.c:907-929` |
| Swap program, transfer bindings, finalize old program | `prepare.c:930-934` |
| Same-count variable handoff | `vdbeapi.c:2064-2077` |
| Release destination, move cell ownership, leave source NULL | `vdbemem.c:1240-1248` |

Modern already has all required ownership primitives:

- one canonical `std::vector<SqlValue>` parameter store in the VM;
- `SqlValue`'s standard-defined, `noexcept` move operations with owned-text
  rebinding and valid NULL moved-from values;
- allocation-free Reset ending execution/register borrows but retaining
  parameters;
- an immutable program plus move-only execution owner; and
- existing session metadata validation before replacement publication.

No parameter container, compiler, cache, borrowed-value factory, allocator
hook in production or compatibility representation is needed.
This is native ownership parity required by current reprepare consumers,
not a preparation timing optimization or permission to change their
transaction-admission behavior.

## Decision

Add one execution-neutral VM operation, `TransferBindingsFrom(Vm&)`, for
already-owned parameter values.

Require two distinct, valid, detached READY VMs with equal parameter counts.
Validate every source logical value against the destination's existing
value-byte limit before changing either store. Return explicit standard
misuse/too-large errors for invalid state, count or value limits; allocation
failure while constructing an error remains the existing typed OOM boundary.
No invalid input publishes a partial handoff.

After validation, move each canonical `SqlValue` into its destination slot
through its existing move assignment. The destination's prior ownership is
released; each source slot remains valid NULL. Preserve slot counts,
programs, limits, execution state and context attachment. Successful
handoff performs zero allocations and does not deep-clone TEXT/BLOB bytes.
Do not swap whole parameter vectors and leave prior destination values
behind in the source.

Keep `Bind` unchanged: caller-supplied values still clone and retain their
existing ready-state, parameter-range and value-limit validation.
Borrowed binding/value views follow the existing mutation/lifetime rules;
register borrows must already be retired by the detached READY precondition.

Both existing facade `Recompile` consumers use the same VM handoff. Prepare
and validate the replacement and parameter metadata first. Transfer only at
the publication boundary, with no subsequent fallible work before replacing
the old execution owner. A failed compile or metadata/limit check retains the
old program and bindings. Never fall back to another clone loop.

No initial Step admission, catalog read owner, schema retry bound,
primary/cleanup precedence, journal requirement, no-op flag, SQL syntax,
public binding semantics, persistent format or later DAG node changes here.

## Two ordered TDD sources

1. VM ownership capability. Add the focused API/behavior specification
   first and retain its expected missing-capability compile red. Implement
   the smallest operation through the existing parameter store/value moves.
   Owning tests cover NULL, integers, reals, embedded bytes, large TEXT/BLOB,
   destination replacement, source NULLs, pointer/lifetime identity,
   zero-allocation success, reuse and destruction. Negative controls cover
   invalid/moved/self, attached/suspended states, unequal counts and
   destination limits, with both stores unchanged on failure.
2. Current facade integration. First reproduce the old actual reprepare
   byte-copy failure using the existing allocation/fault harness and a
   uniquely measured large binding copy. Then replace both current
   recompile loops with the same capability. Cover bound bytes through
   schema change, Reset/rebind, result metadata, replacement failure and
   owned lifetime using the existing session fixtures. Do not relax any
   allocation ceiling or mask a failure to avoid a copy.

Each source requires focused red/green, complete fast units, applicable
ownership/OOM/lifecycle controls, independent SourceAccepted, one normal
commit and append-only ledger evidence before its successor. Publish a
draft after the first accepted prefix under ADR-0106. Qualify the final
exact head with the actual change-aware compiler/sanitizer/native/model,
quality and bounded campaign union, then integrated review, normal squash,
actual-main verification and delivered-reference retirement.

This prerequisite does not close the six separately observed public
admission/preparation-scope mismatches. Resume their completed direct owner
inventory only after this shared helper is qualified. Preserve the native
READY-expired versus actual guard distinction, 50-reprepare runtime bound,
old cleanup before retry and all existing pending completion ownership.
