# ADR-0188: Native Statement Execution Epoch

- Status: Accepted
- Date: 2026-10-31
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `13961a9`, ADR-0173, ADR-0179 and ADR-0187
- Related contracts: ADR-0102, ADR-0106, ADR-0111, ADR-0182, ADR-0184, ADR-0185

## Completed owner inventory

PR #280 closes typed, failure-atomic VM attachment on actual main `13961a9`,
tree `b6a360f`, ledger153. Binding transfer, anonymous savepoint admission,
default ABORT, savepoint playback and READY-expired preparation prerequisites
are qualified. Node43 remains incomplete; unmerged #269 is preserved.

Sixteen fresh controls per engine retain four writable resource-scope
differences. A temporary coordinator Read runs before actual journaled
execution entry; its first OOM loses prior explicit/named work. Native
compiler-selected entry preserves that work. Six raw counter differences
are also observed, but independent first-allocation policies can fail in
different phases. They are not six separately proven counter bugs.

The read facade also refreshes/recompiles before the old guard. Its read
reference is a real execution owner, but replacement compilation currently
skips the shared preparation orchestration. Both data facades lack the
native outer runtime retry mechanism.

### Native state, ordering and cleanup

| Responsibility | Pinned SQLite 3.54.0 source |
|---|---|
| Actual B-tree entry and optional anonymous scope before old guard | `vdbe.c:4245-4350` |
| Guard records expiration and suppresses change publication, including diagnostic OOM | `vdbe.c:4323-4342` |
| READY expiration skips old execution startup | `vdbeapi.c:835-858` |
| Reset retains program expiration | `vdbeaux.c`, `sqlite3VdbeReset` |
| Invalid connection schema has distinct loaded/generation state | `build.c`, `sqlite3ResetOneSchema`; `callback.c`, `sqlite3SchemaClear` |
| Default ABORT and special-error halt policies | `vdbeaux.c:3335-3499` |
| Missing anonymous close is harmless; real close failure is not | `vdbeaux.c:3230-3280,3465-3488` |
| Savepoint rollback allocates playback duplicate membership | `pager.c:3460-3475`; `bitvec.c:116-123` |
| Runtime retry bound50, after old halt | `vdbeapi.c:980-1027` |
| Separate bounded preparation, program swap and binding transfer | `prepare.c:862-934` |

An additional unchanged-native control fails the schema-message allocation,
then resets the target and arms one new first-allocation failure. The first
step returns NOMEM with changes1. The second also returns NOMEM, has no old
PROFILE, preserves changes1 and retains the explicit/named scope. A normal
completion negative control instead reaches actual execution entry on the
second failure and publishes changes0. Source confirms that the difference
is retained expiration, not an allocation ordinal shared between engines.

The canonical Modern seams are:

- `Vm::Impl::ValidateSchema` owns generation/cookie checks, format validation
  and failure-atomic attachment. It currently loses guard origin on diagnostic
  OOM; Reset and returned error codes cannot reconstruct it.
- Session State already owns initialized validity, one catalog, one generation
  producer and ADR-0185's rollback expiration boundary. Invalidity is not a
  fabricated generation or a missing retained snapshot.
- `CatalogRequiresReload` compares the live header with the loaded catalog
  without I/O. Reuse it to invalidate only a genuinely outdated catalog.
- Coordinator admission, writer epochs, statement close, full rollback and
  pending physical cleanup remain canonical. Journal/Pager already replay
  through PageBitvec; no new rollback algorithm is needed.
- `CompilePreparedStatement`, `PrepareReadScope` and `TransferBindingsFrom`
  provide replacement preparation and failure-atomic binding ownership.
- Existing facade error composition retains the primary code and explicit
  cleanup diagnostics. SCHEMA remains primary after old close failure.

## Decision

### 1. Retain one allocation-free VM guard fact

Expose one narrow `Vm::schema_expired()` observation from the existing VM
owner. The canonical validator marks it before allocating a generation/cookie
mismatch diagnostic. Reset, detach, moves and failed replacement preparation
retain that fact. Successful validation of an explicitly supplied valid
lower-level context clears it, preserving the existing direct VM retry
contract; successful facade replacement owns a fresh unexpired VM.

This is the native program-expiration fact translated across Modern's
separate execution/preparation owners, not a second generation, phase enum,
instruction-count heuristic or error-code inference. It is produced only by
the canonical guard. A suspended data-version SCHEMA does not acquire guard
change-publication suppression merely because its code is17.

Carry existing catalog validity through `VmExecutionContext` as an optional
catalog generation. Existing integer-generation calls remain source-compatible
and valid; unavailable generation fails the same canonical guard. Do not
invent generation0, advance identity on another path or pass catalog/session
objects downward. Preserve every snapshot, format, encoding and context
precondition and validate-before-publication boundary.

On an actual guard failure, both facade consumers use `CatalogRequiresReload`
over an existing read snapshot to invalidate only an outdated loaded catalog.
No new I/O, catalog container or duplicated cookie predicate is introduced.

### 2. Admit the old data program before its guard

Keep readiness, preparation cleanup, transaction-command retry and pending
physical completion barriers first. An already-known expired READY program
reprepares before execution, using either the retained guard fact or ADR-0185's
existing rollback boundary.

Otherwise use the old immutable program's actual access, rollback and
statement-journal metadata for one execution admission. Remove the temporary
writable coordinator Read and compile-first preflight from data execution.
The read facade acquires its canonical read reference without loading a
replacement catalog before the old guard.

Attach through the canonical VM validator using existing catalog validity.
On failure, retire VM borrows and close the old execution through the existing
facade/coordinator owner before preparing a replacement. Do not promote a
token, reuse old journal metadata for a new program, change no-op Write access,
or infer zero-work schema provenance from an instruction count.

Ordinary actual execution-entry failures publish changes0 where the existing
program publishes changes, except the native Busy continuation which does not
halt. A real guard suppresses that publication, including diagnostic OOM.
Post-guard execution failures and successful completion retain their existing
counter/last-rowid rules. Keep returned OOM through the canonical allocation
cleanup helper; make its publication decision explicit at the actual phase.

### 3. Share bounded halt-then-reprepare orchestration

Extract one small private runtime retry driver for both data facades. It
executes the existing data path, recognizes returned SCHEMA and permits at
most50 replacement attempts. Each attempt first finishes old execution,
resets the VM, reconciles preparation cleanup and invokes the shared ordinary
preparation orchestration. Its internal preparation schema retry remains1.

Reuse retained SQL, metadata checks and guarded binding transfer. Failed
preparation retains the old program, bindings and expiration; no execution
rollback is performed merely because replacement compilation fails.
Fresh replacement rejoins ordinary admission with its own metadata.

Old statement cleanup failure retains primary SCHEMA and explicit secondary
diagnostics. If canonical full rollback has completed, replacement can run in
the resulting fresh scope, including an implicit transaction. If physical
rollback or read cleanup remains pending, return the composed error and keep
its current owner; neither retry driver nor another statement may prepare
over it. This retains Modern's accepted typed, retryable cleanup boundary
instead of copying native void cleanup's loss of error detail.

Keep transaction commands outside the data retry driver. Source-free programs
do not acquire a read owner merely for retry. The current VM's explicit
generation-validation contract remains; this does not admit new SQL,
multi-connection support, conflict actions or a new persistent transition.

## Three ordered sources and verification

1. Green extraction: separate each facade's existing data execution from
   readiness/automatic Reset/transaction commands without changing order,
   admission, errors, counters or retry behavior.
2. Guard capability: first specify expiry through generation/cookie
   diagnostic OOM, Reset/move and valid lower-level retry in existing VM
   owners. Add optional validity and the single guard fact, then route
   current facade observations/preparation through these canonical contracts.
3. Execution integration: first reproduce the four actual prior-work losses
   through the existing writable OOM owner, with journalfree negative controls.
   Implement actual old admission, shared bounded runtime retry and
   phase-correct publication. Own normal stale execution, schema-message
   OOM, old empty-journal close OOM/full rollback, failed reprepare, Reset/
   rebind, changed replacement metadata, expired/current/source-free programs,
   suspended SCHEMA, named scopes, Busy and pending cleanup in this source.

Keep the fresh16-per-engine scope matrix, native-only Reset/phase controls
and original completed native cleanup observer. Successful qualification must
remove the four scope differences without pretending heap ordinals align.
Use site/phase controls for counters and independent exact rows, reopened
bytes and integrity where applicable.

Each source requires accepted design, genuine focused red/green or a green
refactor baseline, complete fast units, applicable native/sanitizer/owning
failure/lifecycle evidence, strict quality, independent SourceAccepted,
normal commit and append-only ledger before its successor. Publish one
ADR-0106 draft after the first accepted prefix.

Close the final exact-head change-aware compiler/native/sanitizer/model/
quality and bounded campaign union, integrated review, normal squash, fresh
actual-main checks and delivered-reference retirement. No deadline, workload,
assertion, allocation ceiling, default stack or policy setting is relaxed.
