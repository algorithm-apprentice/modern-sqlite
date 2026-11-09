# ADR-0237: Original VALUES Once-Site Ownership

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #311 main `e9724549`, ledger308
- Related contracts: ADR-0106/0111/0218/0219/0221/0226/0231/0233/0234/0235

## Requirement and completed implementation inventory

The retained paired constructor controls have four remaining eager-alias
differences: native executes a deterministic identity callback once, while
Modern executes it twice. Rows agree and neither engine invokes the callback
during preparation. Ordinary/fallback caller modes and two independent
same-text constructors already agree.

Native constructs one VALUES producer before ordinary consumer generation.
Actual source copies retain its generated descriptor, so its function once
sites do not multiply when an alias obtains another consumer. Modern retains
the original bound roots but emits a new ordinary once site in each consumer.
This decision repairs that logical generation-site ownership, not general
expression equality or every remaining native generation mechanism.

The qualified implementation already supplies the required contracts:

| Responsibility | Canonical source |
| --- | --- |
| Original producer, first row and incoming rows | `sqlite3.c:139930-140003` |
| Original producer finalization | `sqlite3.c:139822-139832` |
| Preserve generated state during source copying | `sqlite3.c:114697-114727` |
| Skip regeneration and initialize the retained producer | `sqlite3.c:157552-157560,166123-166133` |
| Reached function gate and retained result | `sqlite3.c:118257-118264,118698-118740` |
| Immutable original root, producer, column and phase | ADR-0233, `BoundValuesGenerationRoot` |
| Consumer rows reuse the successful construction binding | `BindValuesQueryCore`, `BindExpression` |
| Separate actual query instances for semantic copies | `BindExpressionTemplateCopy`, `BindSubquery` |
| Current fresh site and grouped logical replay | `AllocateReachedOnceSite`, `BeginReachedOnceEmission` |
| One iterative value/control/membership emitter | ADR-0231, `EmitOperationLoop` |
| Checked private scratch and result construction | ADR-0218/0221/0234 |
| Existing exclusive invocation-owned callable product | ADR-0226 |
| Existing once epoch, error and reset cleanup | ADR-0219, `Vm::Impl` |

A current sixteen-case binder-only observation confirms that actual aliases
retain their original root IDs, while independent constructors retain
different producers/roots. It includes incoming rows, parameters, multiple
columns, ordered consumers, interleaved producers and discarded queries.
This is diagnostic evidence, not a new behavioral red or a universal
root-ID-sorting guarantee. Its initial unadmitted CASE fixture is retained;
the completed observation uses the existing `iif` conditional.

Ordinary subroutine writes are still attributed to their coroutine callers.
Several inline gates sharing a result across different physical owners are
therefore not the solution. ADR-0226 already proves a uniquely gated,
non-suspending callable's exclusive products without changing VM state.
The original constructor's mutable Yield/return state is not shared here.

## Decision

### Recognize only actual original constructor requests

Scope original-generation tracking around the existing eager VALUES cell
emission, in both streaming and ordered traversal. Consumer-filtered rows
retain the current path. Use the existing original-root inventory from the
common read/write bound owner; add no binder field, syntax copy or second
root inventory.

Use one statement-local compiler index of those original records when
original once generation is needed. Its keys refer to the recorded root's
canonical bound identity, not SQL text, value equality, function name or an
arbitrary ordinary expression. Its values borrow the immutable original
record and own that root's emitted once-site sequence.

Use an ordered associative index with stable element addresses. Index only
the actual original-root population, never configured maxima or every bound
expression. Validate duplicate/invalid original identities as typed internal
errors instead of merging unrelated producers. Do not assume the observed
root IDs are sorted. Initialization is lazy; statements without original
eager requests or potentially reached function roots retain their existing
allocation path.

The producer, column and first/incoming phase remain attached to the original
record. A consumer chooses that original record, not a replacement producer
derived from its fresh query ID. Plain/nonconstant roots gain no runtime
cache merely because they belong to the same constructor.

### Share a generation sequence, not expression values

Reuse the current ordered once-event traversal and canonical site/result
allocator. Generalize its compiler bookkeeping only enough to distinguish
the existing inline grouped form from an original callable form.

The first physical emission of an original root records admitted once events
in the existing continuation order. A later consumer of that same root
replays the sequence. Retain the expression identity at each event and
require both identity and final event count to match. Repeated occurrences
remain separate sequence positions, including repeated same-valued calls
within one root. Empty definitions must not displace other definitions.

Original generation is the innermost owner while an eager cell is emitted.
Restore the enclosing traversal on every exit. Grouped output outside that
cell retains its existing definition key, stable storage, inline bodies and
replay behavior. Do not broaden grouped ownership across private regions.

### Give each original once body one physical writer

For an original event, allocate the existing retained result and once ID,
register one existing read subroutine and retain its entry/return labels.
Call sites use the existing typed call and copy the retained result into
their requested destinations. Share the call/result-copy helper with the
current uncorrelated-query caller; do not introduce another evaluator.

Emit each definition once, as a complete private region after ordinary query
and coroutine definitions and before the existing entry initializer. Visit
the canonical original records and their recorded event order when draining
these bodies. Keep the original root/event identities independent of physical
placement or the first consumer's query ID.

Each body starts with its unique `OnceInstruction`, whose already-seen edge
targets the matching final return. Generate the same admitted expression
through the current iterative emitter with factoring disabled. Use the
existing private expression-register owner for that subroutine. Borrow the
first requesting compiler frame's existing immutable environment/services;
do not fabricate a bound query or transfer a consumer's mutable registers.

ConstantNotJoin admission remains unchanged. It excludes columns, queries
and aggregates. Existing scalar control and list-membership emission create
their resources through the same builder, including reusable membership
relations owned by this read body. No nested callable, coroutine instruction
or destructive move may be introduced into the proven invocation product.
Keep every ordinary capability, initialization, return and ownership check.

The result home is separate from coroutine outputs, receivers, argument
blocks and affinity scratch. Append it once to the existing protected NULL
construction list. A replay adds no initializer or second writer.
Runtime failure, interruption, reset/rebind, row suspension and independent
VMs retain the existing epoch/register/capability cleanup.

This narrowly changes ADR-0234's physical-emission identity for the original
sites retained by ADR-0233. It does not activate the non-function deferred
queue or reusable-expression comparison, compile an unused SQL callback at
preparation, move binder diagnostics, or claim full native preparation order.
The later physical body placement must preserve all retained preparation
outcomes; any newly observed winner change blocks this slice.

### Preserve bounded storage and explicit failures

Store compiler metadata in the existing statement lifetime. Definition and
site references must survive nested registry growth. Retain only actual
original records and emitted events; allocate no per-scope expression matrix,
runtime value map or configured-maximum buffer.

Use checked builder allocation and existing typed errors for invalid metadata,
labels, register counts and resource exhaustion. Normal compiler allocation
failure publishes no program and retires all partially collected state.
Do not change verifier recognition, instruction budgets, allocation ceilings,
workloads or timeouts.

Record actual compiler-state, lowering-allocation, program-resource and
fixed-work costs against the qualified baseline. Additional call/return
instructions are an ownership cost, not a claimed speed improvement.
Preserve existing non-target exact-allocation contracts.

## Focused proof and delivery

After independent design acceptance, add the existing eager-alias case to
the owning lowerer regression and record the current two-versus-one failure
before production changes.

Cover captured/uncaptured aliases, disabled IN consumers, ordinary fallback,
independent same-text constructors, several once events in one original root,
incoming rows, two original cohorts, ordered/private consumers and grouped
copies. Include list membership inside a constant function under both
temporary-storage modes, plus nondeterministic/column-dependent controls.

Exercise generated read/write hosts, reset/rebind, early stop, owned TEXT/BLOB
results, callback failure and interruption. Reuse existing ownership-negative
tests rather than loosening them. Extend the owning allocation-failure cases
only for newly exercised paths and keep its exact policy population aligned.

Retain the original eight paired controls, wider contextual controls and
115 native/write plus103 read preparation protocols. Reconcile only exact
unchanged inputs; rebuild observers against changed consumed definitions.
Run focused green, complete fast units, current compiler/checker owners and
the actual change-aware closing plan. Independent source/integrated reviews
and normal exact-head hosted/main delivery remain required.

This is one coherent original-site correction under node43. General deferred
and signed generation, preparation-error arbitration, IN query RHS and later
DAG nodes remain separate work.
