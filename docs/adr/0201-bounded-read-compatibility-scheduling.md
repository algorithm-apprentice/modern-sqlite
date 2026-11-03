# ADR-0201: Bounded Read Compatibility Scheduling

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Related decisions: ADR-0038, ADR-0094, ADR-0102, ADR-0106, ADR-0165
- Design accepted before implementation at proposal `822567f7`.
- Tool-owner resource refinement accepted before implementation at `b22018a1`.

## Measured qualification failure

PR #290's exact-tree premerge gates passed, but actual-main CI run
`41590517201` failed: the sanitizer `differential.read_compatibility` owner
exceeded its unchanged 60-second deadline at 60.07 seconds. Six following
owners were not reached. Other successful jobs do not replace that failure.
The same-tree premerge owner took 51.91 seconds.

Preliminary cached-image controls expose substantial serial process-lifecycle
cost and complete a private two-slot experiment within the original deadline.
The subsequent build audit finds that this ASan trace executable was last
linked on October 29, 2026: the previous source qualification built its
selected unit/model owners, not this separate trace target. Those controls
therefore are not a source-matched reproduction of November 3's main and
must not qualify this correction.

The failed hosted run remains the actual-main failure evidence. Rebuild the
trace target and record its real image identity before current-source serial
and two-slot qualification. Preserve the preliminary observations with their
cached-image scope, rather than relabeling them as fresh execution or inferring
a hosted hardware cause. Neither a private experiment nor an old successful
gate replaces the required current-source results.

## Canonical mechanism and scope

Reuse ADR-0038's complete corpus/oracle validation, MSRT1 protocol, one
independent process per trace, canonical outcome validation, comparison and
failure classification. Reuse `_run_bounded` for process creation, temporary
input, bounded pipes, the absolute child deadline, termination and reaping.
The existing owning test retains both native-valid unsupported and ordinary
prepare-error controls.

This measured alternative changes verification scheduling, not SQLite's
engine algorithms, SQL semantics, production library, fixtures, corpus,
compiler options, sanitizer checks or benchmark policy. No second verifier,
process-reuse protocol, session cache or engine batching is introduced.

## Decision

`verify` accepts an explicit `--jobs` value of one or two, defaulting to one.
Invalid values fail before runner launch. Regeneration remains sequential.
The single-slot path creates no executor and retains the existing capture
behavior. Concurrency facilities are loaded only by the two-slot path.

The two-slot path uses one standard-library executor and an ordered window
of at most two admitted futures. Each invokes the same isolated runner once.
The consumer receives corpus order, not completion order. The window advances
only after the consumer accepts the preceding comparison. The next
unconsumed case may have one speculative successor; the whole corpus is
never submitted eagerly. The consumer's current result is separately bounded.

The outcome iterator is explicitly closed on every exit. Closing cancels
queued work, requests cancellation of active work, and joins workers before
returning. Already-admitted work may race with cancellation and must still
be cleaned up. The earliest failing case in corpus order determines the
reported failure; a later worker's failure or speculative cancellation never
replaces it. No success summary is printed before every selected case has
been compared.

An optional private cancellation predicate extends the canonical runner.
Pre-cancelled work acquires no process resources. Cancellable selector and
post-EOF process waits use bounded polling slices against the same absolute
deadline; a polling-slice expiry is not a child timeout. Cancellation raises
an explicit harness error through the existing cleanup, which kills and
reaps only the owned process and closes its input and capture resources.
Cancellation also covers a child that closes both pipes but remains alive.

The seven existing ASan native-differential owners centrally select two
jobs and declare `PROCESSORS 2`, retaining their existing `RUN_SERIAL` and
60-second deadlines. They use the existing CI two-slot reservation, not
another CTest worker or larger machine/job envelope. The seven non-ASan
native-differential commands and properties remain unchanged. Explicit
two-slot operation is also qualified on native Windows.

The verifier unit owner itself reserves `PROCESSORS 2` on every platform:
its new two-worker fixtures must not share a single declared slot with
another owner. A complete native-preset failure and same-source isolated
control exposed this missing resource declaration. Its command, labels,
10-second deadline and existing CLI work remain unchanged; it gains no
`RUN_SERIAL` property. This is the only additional cross-platform
registration change.

The 512-case cap, every operation and assertion, independent child processes,
10-second child deadlines, capture limits, protected lowering/OOM resources
and 900-second sanitizer job envelope remain unchanged.

## Provenance and verification

Update the generator digest through the existing pinned profile and
canonically regenerate all seven oracles with the pinned native library.
Only generator/profile provenance may differ: all SQLite configuration,
fixtures, cases, operations, outcomes and boundaries remain identical.
Existing authenticated profile history handles historical references;
neither archives nor validators are relaxed.

Test-first controls cover actual two-slot CLI verification, bounded
admission and overlap, inverse completion with ordered first failure,
active cancellation and post-EOF cancellation, reaping, unchanged absolute
deadlines and capture bounds, invalid worker counts, and both failure
classifications. Keep complete fast units, owning Python/policy suites,
actual discovery reconciliation, native Windows/GNU execution and native
oracle regeneration.

All seven real ASan owners must pass their original limits on the accepted
implementation. Required exact-head hosted/native/quality gates and
independent source/integration review remain mandatory. The failed ancestor
run remains failed; only a freshly qualified corrective main closes this
delivery and permits exact-ref retirement. General name/value-generation
prerequisites, IN admission and later DAG nodes remain open.
