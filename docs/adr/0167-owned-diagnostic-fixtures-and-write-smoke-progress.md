# ADR-0167: Owned Diagnostic Fixtures and Write-Smoke Progress

- Status: Accepted
- Date: 2026-10-29
- Module graph node: `implement-advanced-sql`
- Related decisions: ADR-0106, ADR-0153, ADR-0166
- Predecessor: reviewed PR #269 commit `c34b7c9`, tree `c32a1da5`
- Refinement accepted before implementation at proposal `58c45637`.

## Actual qualification findings

The preceding commit passes all 2,139 native units and ten interoperability
owners, all 2,117 Linux sanitizer units and two interoperability owners, and
independent source review. Exact-head Full quality run 40856918929 passes
all four jobs. CI run 40856921768 retains two failures:

- Native Windows completes the previously timed-out hot-journal case, then
  stops at `fuzz.write_smoke` under its original 20-second deadline. Its
  captured output does not identify a corpus input or generated control.
- macOS Debug reports an unexpectedly healthy inspection for the deliberately
  changed schema type in
  `StorageInspector.ValidatesSchemaRootKindsAndSuppressesIncompleteReachability`.
  The existing assertions remain required.

Neither failure is a passing gate. No specific blocked operation or original
failure cause has been established. A bounded local concurrent characterization
of the inspector case and its adjacent control does not reproduce the failure.

## Existing ownership and progress inventory

The diagnostic fixture helper currently uses a clock value and a process-local
counter followed by a truncating open. Those values do not reserve a filename
across processes. This is a concrete ownership weakness, not proof that the
observed macOS failure was a collision.

The fuzz consumers already share one `PrivateDatabase` owner: POSIX uses
`mkdtemp` and exclusive creation; Windows uses an exclusively created private
directory, retained directory handle and exclusive database creation.
Reuse that owner rather than add another temporary-file implementation.
ADR-0166's shared `PhaseTiming` is the canonical diagnostic clock.

## Ordered decision

Use two further coherent commits in the same related qualification PR,
for three commits total under ADR-0106.

1. Promote the existing private database owner to shared test support and
   update its existing consumers without changing its creation, writing or
   cleanup algorithms. Use it for diagnostic fixtures instead of clock-named
   truncating files. Verify fixture bytes before inspection; retain every
   corruption mutation and assertion, and include the actual inspection
   report in failed-assertion context. Add ownership, independent-image and
   cleanup controls. This closes the ownership weakness, not an asserted
   diagnosis of the earlier failure.
2. Add immediately flushed per-input and generated-control progress to the
   existing write-smoke driver, using the same timing kernel. Retain corpus
   enumeration/order, every input byte, both fuzz consumers, all bounds,
   session/transaction behavior and the whole 20-second deadline.

No production library, VFS, storage algorithm, SQL surface, worker, scheduler,
timeout, assertion or fuzz campaign is replaced or reduced. Do not select
weaker synchronization, a smaller corpus or a success-shaped fallback.
New failures retain their actual fixture/progress evidence before any
further behavioral change.

## Validation and delivery

Review this refinement before implementation. Each commit retains focused
behavior or a green ownership-refactor baseline, complete fast units,
independent implementation acceptance and append-only ledger evidence before
its successor begins. Validate actual CTest names, commands, labels, resource
ownership and deadlines, allowing only added fixture-control cases.

Run the combined actual change-aware closing gates against the final exact
head. Keep the two failed contexts explicit until their original complete
selections actually pass. Squash only after integrated acceptance, then verify
actual main and clean only the delivered local and remote branch.
Literal/name/unary/scalar-width and IN work remain paused; node 43 and the
project remain incomplete.
