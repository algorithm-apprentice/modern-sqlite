# ADR-0175: Bounded Native B-tree Compatibility Processes

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0106, ADR-0153, ADR-0174

## Observed qualification failure

ADR-0174's three production sources are independently reviewed and locally
qualified. At final head `5871611`, all four actual manual Full quality jobs
pass, including both complete runtime configurations and the four original
20,000-run, seed-102 campaigns. Eight automatic CI contexts pass. Native
`compatibility.btree_writer_sqlite` instead reaches its unchanged 30-second
deadline at 31.36 seconds. Only 2,197 of 2,222 selected native cases execute;
the following 25 cases are not evidence.

The original serial fixture completes only three of sixteen geometries in
29,331 milliseconds. Their pinned-SQLite create/mutate phases account for
23,819 milliseconds, versus 5,500 for Modern's create/mutate phases. The
earlier first-source head also timed out in this owner; the second-source
head passed. These observations identify deadline-sensitive fixture work,
not a demonstrated engine defect or a particular hosted filesystem syscall.
The exact hosted operating-system stall remains unattributed.

The exact unmodified final-source native executable is preserved. A
diagnostic translation unit includes the unchanged fixture and links the
unchanged crash-campaign object and native Debug libraries. Its delegated
native VFS observes all sixteen geometries, 160 geometry SQL executions,
128 geometry autocommit writes and 456 native synchronization calls for the
whole owner. The geometry statements account for 384 syncs; the original
crash campaign's fixture construction accounts for the remaining 72.
With zero injected latency it passes. Adding 350 milliseconds before each
real delegated native synchronization reaches the same 30-second bound
after three completed geometries. This is controlled scheduling
characterization, not a qualifying run or proof of the hosted syscall's
latency.

Do not batch the five native create statements or three native mutations
into fewer transactions, change durability, move setup outside the owner,
copy prebuilt fixtures, increase the deadline or rerun a failed head.

## Canonical inventory and independence

The fixture already has one canonical `RunGeometryCase`, sixteen geometry
identities and geometry-specific database names. Each case completes both
interoperability directions and retains schema, root, row, covering-index,
freelist, geometry and native integrity assertions. Cases share no database
or cursor. The crash campaign has ten independent operation fixtures. Each
is constructed once, then runs both durability modes and every mutation cut
through its existing recovery and full-image verification. The complete
baseline manifest contains 112 geometry-phase and 1,730 crash-phase keys.
An earlier diagnostic parser omitted hyphenated operation names; that
partial artifact remains preserved and is not the complete manifest.

The pinned native library is built with `SQLITE_THREADSAFE=0`. Independent
SQL connections must not be made concurrent within one process. Do not copy
ADR-0171's compiler threads into this native-library workload.

`tests/support/windows_process.hpp` already owns native child processes,
uses the canonical `WindowsHandle`, preserves child diagnostics, restricts
inherited handles, waits five seconds by default and checks termination.
Extract its launch/wait implementation into one owned native-process type
in that same header. Keep the existing synchronous function as a wrapper
over that owner, not a second launcher. The new asynchronous owner is needed
to start all eight independent processes without adding eight parent
threads, and to observe child cleanup while the outer job is still alive.
The native platform owning tier retains its existing dependency boundary;
fixture controls belong to a separate B-tree target, not the VFS target.

## Decision

### One unchanged owner, eight bounded native process lanes

On Windows only, distribute the original geometry indices and the ten whole
crash-operation owners round-robin over exactly eight child processes.
Each child runs two complete geometries followed by its one or two original
crash fixtures. Keep one
`compatibility.btree_writer_sqlite` owner with the same executable command,
label and whole 30-second deadline. Reserve eight CTest processor slots;
these are bounded I/O lanes, not a single-CPU engine timing comparison.
There are no independently permitted 30-second shards.

Use the same geometry and crash dispatchers for serial and child execution.
Keep existing SQL, all eight native autocommits per geometry, default native
durability and every verification assertion. Do not construct twenty
per-mode crash fixtures or change either mode's cut population.

The Windows parent uses the existing canonical `WindowsTemporaryDirectory`;
add only the narrow root-path borrow needed by its consumers. Geometry
databases, disjoint operation fixture/recovered paths and lane diagnostic
files all belong to that one root. Replace the crash helper's redundant
directory owner with a borrowed filesystem path. Children borrow a validated
ASCII directory leaf under the inherited native temporary root and never
remove the parent directory. Invalid identities, leaf names or missing
directories fail explicitly before database work.

Each child uses its own pinned-library global state. Its exit success means
all assigned geometry and whole crash-operation cases completed. Start all
eight owned processes, then join every started lane and reject every failed
or missing result. There is no serial native-I/O tail in the Windows parent.
A child failure is not a successful skip. Non-Windows execution retains
the original serial geometry, operation and durability order.

Each lane receives its own noninherited diagnostic file under the parent
root. The canonical launcher duplicates only the explicitly selected
standard streams into the existing handle allowlist; it does not change
global standard handles. After joining, emit the complete lane logs in
deterministic order before reporting failures. No concurrent stdio
interleaving may hide the work manifest or child diagnostics.

Eight lanes are a fixed local bound, not a configurable pool. The proposed
round-robin work has source-derived native-sync loads of 60, 63, 54, 54,
54, 54, 57 and 60. At the stronger 350-millisecond diagnostic envelope the
largest native-sync contribution is 22.05 seconds. This is a candidate
budget, not measured scheduling evidence or a guarantee about arbitrary
host latency. Measure each complete lane and parent before acceptance.
Leaving the 72 crash syncs serial would add 25.2 seconds and is rejected.

### Retain explicit child ownership

Keep the canonical launcher's five-second default for every existing caller.
The compatibility owner alone has one common 25-second deadline measured
before lane creation. Each join receives only its remaining time, never an
additional 25 seconds. The whole existing CTest owner still allows only
30 seconds. Preserve explicit handle allowlists and checked termination.

Own each launched child with a noninherited, close-terminating Windows job.
Supply that job through `PROC_THREAD_ATTRIBUTE_JOB_LIST` in the same process
creation attribute list as the existing handle allowlist. Do not introduce
the parent-exit window of creating a suspended child and assigning its job
afterward. Fail explicitly if required job or process setup is unavailable;
do not add an older-system fallback, detach children or change inherited
parent-job restrictions.

The one native-process owner provides start, process identity and checked
wait; its destruction closes the owned job and joins its direct child.
The existing synchronous helper reuses that implementation. A wait failure
or exception retires and joins every launched lane before the parent root
is released. An unjoinable child is a failing owner, never silent cleanup.

### Focused controls before scheduling

Add a native B-tree process test target using the existing GoogleTest and
native process helper. First demonstrate that the old fixture incorrectly
ignores an invalid worker request and exits successfully. Then cover
malformed/out-of-range identities, invalid or missing parent directories,
child failure propagation, the existing default wait, and an explicit short
wait with owned cleanup. Add an abrupt-parent-exit control: the observer
retains its own outer job while the probe parent exits without C++
destruction; its running child must terminate through the probe's own job.
Do not let closing the observer's outer job manufacture that result.
Keep these controls out of the platform-only early-build targets.

## Qualification and delivery bound

Before implementation require independent design acceptance. Preserve the
original whole-owner failure and exact binary. Compare the complete
before/after populations: all 112 geometry-phase keys, 1,730 crash-phase
keys, ten fixture constructions, both modes and every cut. Reconcile all
160 geometry SQL executions, 128 geometry autocommits and 456 whole-owner
delegated native syncs across children and parent under the unchanged native
configuration. The 350-millisecond diagnostic must retain all that work,
keep each complete lane inside the common 25 seconds and finish inside
the original 30 seconds. It does not replace ordinary qualification.

Require focused native controls, complete native fast units and the whole
original owner, strict native changed-test/header analysis, the selected
sanitizer/model/interoperability union, and all final exact-head hosted
contexts and actual manual jobs. No original deadline, workload, allocation
ceiling, persistent assertion, crash cut or compiler/sanitizer policy changes.

Deliver this localized final-gate correction as the fourth and final normal
commit in #273. It changes no engine, VFS, pinned source, SQL semantics or
persistent transition. Another significant finding requires a reviewed split
under ADR-0106, not a fifth accumulating commit. All integrated evidence and
review must be requalified on the new exact head before normal squash merge.

## Windows ownership references

- Microsoft Learn, `UpdateProcThreadAttribute`: creation-time job list and
  handle list, supported on Windows 10 and Windows Server 2016 or newer.
- Microsoft Learn, `Job Objects`: final close of a close-terminating job
  retires its associated processes, including nested descendants.
- Microsoft Learn, `Nested Jobs`: preserve inherited job restrictions.

## Local native quality reconciliation

Full changed-test analysis exposes an MSVC standard-library bitmask
diagnostic through the unchanged crash image reader's `file_size` call.
Exporting exact `5871611`'s original crash CPP and matching header reproduces
that diagnostic with the same native compiler arguments and project checks.
Do not suppress the checker or describe this as an engine format failure.

Use the existing fixture tooling's single-open stream sizing mechanism for
this bounded reader: open at end, validate the position and original
deterministic-VFS/size bounds, allocate the same `ByteBuffer`, seek to the
beginning and read every byte. Retain all SQL and image assertions. This is
a green-baseline test-intake refactor, not a new persistent mechanism or an
allocation-limit change.

The new probe entry point must also keep its error-reporting handler from
throwing: a C++ error stream can allocate or raise another I/O exception
while handling the original failure. Report through bounded native writes
instead, retaining an explicit failing exit status if reporting itself
fails. Add a child control with a failed C++ error stream to exercise that
boundary. Keep every configured analyzer check and warning policy.
