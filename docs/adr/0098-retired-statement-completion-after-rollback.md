# ADR-0098: Retired Statement Completion After Rollback

- Status: Accepted
- Date: 2026-10-16

## Context

The implemented-engine reference audit's D2 reproduction reaches an existing
session/coordinator seam. An implicit DML VM finishes, but its pre-durable
commit fails. The coordinator successfully performs automatic full rollback
and retires the transaction statement. The session nevertheless retains
`success_pending`, its owning statement wrapper, unpublished catalog, and
pending change count.

For a second INSERT after a successful first INSERT, the database correctly
retains only the first row and returns to clean autocommit. However,
`changes()` remains one instead of zero, and Reset retries `Succeed()` on
the retired token. That stale-token error prevents normal reuse of the
prepared statement.

Pinned SQLite clears a failed implicit DML's change count in
`src/vdbeaux.c:3412-3453,3489-3498`. Its `sqlite3_reset()` returns the prior
execution error but still rewinds the statement in `src/vdbeapi.c:126-149`.
An independent native BUSY control confirms zero changes, the prior error
from Reset, and successful later execution of the same prepared statement.

The existing canonical mechanisms are:

- `TransactionStatement::valid()`, a nonallocating, nonthrowing query of the
  coordinator's authoritative token lifetime;
- the coordinator's existing automatic full rollback and terminal cleanup,
  which retire that token only after their own successful completion;
- one session-owned pending-completion state: statement wrapper, pending
  success flag, candidate catalog, and pending change count;
- the existing VM mutation-result metadata, which distinguishes DML change
  publication from DDL and read statements;
- one retained execution error and the ordinary reset/finalize/automatic
  reexecution lifecycle; and
- the existing fixed VFS fault controls and writable-session test helpers.

An error does not by itself mean rollback completed. In particular, a durable
commit with only `kWriterFinished` or end-read cleanup remaining still has a
valid transaction token and must retain its pending publication for retry.

## Decision

### 1. Reuse authoritative token retirement

After `TransactionStatement::Succeed()` returns an error, inspect the same
statement's existing `valid()` query. If the token remains valid, retain
the existing pending-completion ownership and return the original error.
Do not classify it using error text, an error-code whitelist, autocommit,
or a separately reconstructed Pager state.

If the token has retired, the coordinator has completed its terminal work.
The session must discard the pending completion rather than invoke another
operation on that token. For the reproduced failure this means automatic
full rollback has completed, not that the original VM mutation succeeded.
Return the original typed completion error unchanged.

Do not add another token, retry flag, transaction state machine, completion
registry, owning snapshot, or public API. Coordinator and Pager production
code remain unchanged.

### 2. Clear completion ownership through one canonical operation

Use one private session operation to clear:

- the owning transaction-statement wrapper;
- `success_pending`;
- the unpublished candidate catalog; and
- the pending change count.

Reuse it at the existing successful rollback cleanup and terminal completion
sites instead of duplicating the same state reset. It performs no I/O,
allocation, commit, rollback, or error construction.

On failed completion with a retired token, publish zero connection changes
only when the existing program metadata says this is a DML change publisher.
A failed DDL/read does not overwrite the previous successful DML count.
Discard its candidate catalog and pending count rather than publishing them.

`last_insert_rowid()` keeps its existing earlier VM publication point.
Automatic rollback does not undo an INSERT event, matching native behavior.
Do not conflate it with statement-success change publication.

### 3. Preserve successful and retryable completion

On ordinary successful completion, publish the validated candidate catalog
and change count through the existing session ownership, then retire the
same pending-completion state. Do not publish either after an error.

A still-valid token after an error retains the existing coordinator-owned
terminal operation. In particular, durable DML/DDL cleanup retries must
retain their count and catalog candidate until completion succeeds. The
correction must not roll back durable content, discard a retained schema,
rerun its VM, or publish a successful result before cleanup completes.

This decision does not reinterpret a still-valid error as successful
rollback or invent new retry semantics for unfinished coordinator work.
The existing typed error and ownership remain authoritative.

### 4. Let canonical lifecycle operations reuse the prepared statement

Once a failed completion has retired and its pending state is cleared:

- Reset resets the existing VM, returns the prior execution error, and
  still makes the statement ready for rebinding or reuse;
- automatic Step reexecution uses the existing non-reporting reset path
  and begins a fresh transaction statement, never the retired token; and
- Finalize returns the original error and releases the prepared statement
  without appending a stale-token cleanup error.

Bindings, parameter metadata, immutable bytecode, catalog generation checks,
automatic reprepare, and zombie-session ownership remain canonical.
The prepared statement remains valid until finalized; that public validity
must not be confused with its private transaction token's validity.

## Verification and delivery

This is one prerequisite correctness slice of `implement-advanced-sql`,
after ADR-0097/PR #206. Node 43 remains active; ADR-0092 expression-copy
behavior and later DAG nodes do not begin here.

Obtain GPT-5.6 Sol design approval before production changes. First add and
observe one public-session failure for a VM-complete implicit INSERT whose
EXCLUSIVE commit lock fails and whose automatic rollback succeeds. Check
the old image, autocommit, zero changes, retained last-insert-rowid, original
Reset error, and actual successful reuse.

Cover BUSY and typed I/O completion failures, rebinding, automatic Step
reexecution, direct finalization, pending DDL candidate rollback, and DDL's
unchanged prior DML count. Add controls for durable DML and DDL cleanup
failures at both shared-lock downgrade and end-read unlock, proving the
existing pending count/catalog retry is retained and no VM mutation repeats.
For the latter, the shared fault control skips the initial catalog-probe
unlock and fails the matching terminal unlock instead; a catalog-refresh
failure is not evidence that VM commit cleanup was exercised.
Keep ordinary explicit transaction, constraint, reset, and rollback tests.

Exercise the new error-cleanup seam under the existing allocation-failure
runner without increasing ceilings or introducing a second allocator.
Run the focused tests and complete fast unit tier after the correction.
Use pinned native BUSY/reset/reexecution controls to compare changes,
last-insert-rowid, autocommit, rows, schema, and original error reporting.
Native lock contention is only an oracle control; Modern multi-connection
scheduling remains deferred.

No persistent transition or format changes. Retain the existing crash,
golden, native interoperability, model, and performance evidence. Run the
complete local compiler/sanitizer matrix, isolated native contracts, static
and project gates, and the inherited source-work properties. Preserve
workloads, fixtures, oracle profiles, resource controls, allocation ceilings,
performance thresholds, and every deadline, including the original
correlated sanitizer model.

Require GPT-5.6 Sol implementation and exact-tree review, then all nine
new-head hosted checks before normal squash merge. Verify the merged main
tree and clean only the confirmed merged branch.

### Focused development evidence

The initial public-session red was observed against unchanged production at
main `b6e5e29904e2a769ae13b9f9de2bc8e2f5566f3d`. The expanded,
phase-correct suite observed six failing lifecycle regressions while both
durable DML/DDL cleanup controls already passed. The new fail-all allocation
Reset regression also rejected the old implementation.

The canonical correction passes all 27 focused writable-session/allocation
cases and the complete 1,472-case Debug suite. The durable controls preserve
pending publication across both shared downgrade and end-read cleanup,
without another database write. The retired Reset returns its original
error even while every new allocation is rejected.

Seven independent public lifecycle transcripts match pinned SQLite:
Reset, rebinding, automatic Step, direct Finalize, DDL, UPDATE, and DELETE.
They compare the BUSY result, changes, last-insert-rowid, autocommit, old
and final rows, schema visibility, cleanup error, and later completion.
Native reads every final Modern image, and all fourteen final Modern/native
images pass native `integrity_check`. These results do not replace the
remaining compiler/sanitizer, native-contract, static, exact-tree review,
and hosted delivery gates.

## References

- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0048: Writable SQL DML and CREATE TABLE Execution
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- [Implemented-engine reference audit](../sqlite-reference-audit.md), D2
- Pinned SQLite `src/vdbeaux.c:3412-3453,3489-3498`
- Pinned SQLite `src/vdbeapi.c:126-149`
