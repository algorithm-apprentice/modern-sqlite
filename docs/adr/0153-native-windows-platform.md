# ADR-0153: Native Windows Platform Prerequisite

- Status: Accepted
- Date: 2026-10-25
- Module graph node: `implement-advanced-sql`
- Predecessor: PR #258, accepted main `f648a84`

## Context and ordering

The project owner is developing on Windows and prioritizes a native Windows
development and execution path before the next scalar IN query-RHS admission.
The existing Windows CMake configuration can succeed, but the production
library unconditionally compiles `posix_vfs.cpp`, which deliberately rejects
Windows. Session defaults and otherwise platform-independent tests also name
`PosixVfs` directly.

The owner clarified that native Windows means the installed Visual Studio
MSVC/VC++ toolchain. MinGW is not a supported delivery target; its earlier
experimental builds do not justify a second Windows compiler maintenance
matrix. Linux GCC/Clang development remains available through WSL, with the
existing Linux/macOS hosted matrix unchanged.

This is one bounded platform prerequisite within the active node, like its
earlier platform corrections. Add its acceptance gate to the current graph
node; do not mark advanced SQL complete, renumber the established nodes,
start concurrency/WAL/C API, or reopen the closed implemented-engine audit.
After this prerequisite merges, resume PR #258's IN query-RHS handoff.

Use the current enlistment, not another worktree. WSL remains a Linux
regression environment, not Windows execution evidence. No claim that native
Windows is faster follows from this admission. Benchmark expansion and timing
remain deferred under ADR-0053.

## Completed implementation inventory and reference map

`Vfs` and `File` already own typed options, range validation, short-read zero
fill, explicit errors, and instrumentation. Pager, journal, B-tree, temporary
storage and both sessions consume that boundary. `PosixVfs` remains the one
Unix backend; its process-owned `fcntl` registry must not be copied into the
Windows handle-owned database-lock implementation.

Use the fixed SQLite 3.54.0 checkout at Git `cb547ab3e931c7766e24834af5ef6c4578863e3d`,
Fossil `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
The generated oracle inputs retain the hashes in the development handoff.

| Mechanism | Pinned `src/os_win.c` |
|---|---|
| Native error capture and bounded transient-I/O retries | 1436-1600 |
| Handle-owned byte-range lock/unlock and signed file offsets | 1600-1767 |
| Close, offset-based reads, complete writes, EOF handling | 1784-2001 |
| Truncate, file size and `FlushFileBuffers` synchronization | 2010-2245 |
| Shared/reserved/pending/exclusive database-lock transitions | 2260-2580 |
| Sector size, undeletable-open files and powersafe-overwrite policy | 2810-2841 |
| UTF-8/UTF-16 filename boundary and long names | 1100-1433,2848-2930 |
| Process-wide shared mapping owner and per-connection lock handles | 2954-3040,3134-3390 |
| First-attachment deadman lock and stale shared-memory truncation | 3094-3130,3598-3613 |
| Shared-memory lock masks, remote-volume shared-handle policy and barrier | 3250-3282,3404-3568 |
| Allocation-granularity-aware mapped-region growth and cleanup | 3051-3086,3586-3720 |
| Temporary directory, randomized names and delete-on-close opens | 4078-4250,4290-4499 |
| Retried file deletion and Windows directory-sync policy | 4525-4577 |
| Nonempty-file existence and read/write access observations | 4584-4667 |
| Absolute Unicode paths, including absent final components | 4796-4964 |
| Randomness, rounded sleep and wall time | 5048-5154 |

The ordinary Windows desktop mechanisms are the first admission. Win9x,
WinRT/UWP, Cygwin, URI file controls, extension loading, data-file mmap, and
blocking-lock timeout modes remain outside the existing public contract.
Shared-memory primitives do not admit SQL WAL or later-node concurrency.

## Decision

### 1. One native backend selection

Add `WindowsVfs` under the existing platform boundary and a compile-time
`NativeVfs` alias selecting it on Windows and `PosixVfs` on Linux/macOS.
CMake selects exactly one production backend source. Unsupported systems fail
configuration explicitly. There is no runtime registry, plugin, POSIX
emulation, or Windows-specific SQL/storage implementation.

Both session defaults and the diagnostic tool use this selection. Generic
file-backed tests use the same selection; genuinely POSIX syscall, permission,
symlink and process-lock tests remain POSIX tests. Windows has owning tests
for its corresponding native behavior rather than pretending that Unix
permissions or handle lifetimes are interchangeable.

### 2. Preserve native Windows I/O, locking and durability

Use Unicode Win32 APIs at the UTF-8 VFS boundary. Reject malformed UTF-8,
unrepresentable signed 64-bit offsets and invalid backend state explicitly.
Retain immutable filenames and move-only RAII ownership for every OS handle
and mapped view. No owning C++ heap pointer escapes an owner.

Read/write through offset-bearing `OVERLAPPED` values on ordinary synchronous
file handles, as in the reference. Bound each transfer to the API's count
range, preserve complete-write retries, and let the shared `File` front door
zero-fill short reads. Do not add successful-I/O allocations.

Translate the native ten-retry, increasing 25 ms transient-I/O policy and
three-attempt pending-lock acquisition. Capture the original OS error before
cleanup or further API calls. Expected errors remain typed results, including
contention, read-only access, disk full, invalid paths and allocation failure.
Explicit unlock, mapping and cleanup calls report failures; destruction alone
is best effort and never implies synchronization.

`Sync` uses `FlushFileBuffers` for both accepted modes. Windows deletion
follows the native `DeleteFileW` protocol; the directory-sync argument does
not invent a Unix directory `fsync` on Windows. This platform-specific meaning
is documented, not a silently omitted requested operation. Named open files
are not shared for deletion. Report the native undeletable-while-open and
subpage-read characteristics, plus powersafe overwrite for main databases;
do not claim atomic writes or safe append.

Database locks use the same pending/reserved/shared byte ranges and
`LockFileEx`/`UnlockFileEx` ordering. A failed RESERVED-to-EXCLUSIVE writer
upgrade retains the pending fence and restores the shared lock. Subsequent
readers remain blocked until downgrade or close. Preserve the distinct native
direct SHARED-to-EXCLUSIVE recovery attempt, which does not first reserve
the pending byte. A native VFS callback control confirms that a failed direct
attempt allows another SHARED reader and reports no external reservation,
whereas the ordinary writer path blocks that reader and reports reservation.
Do not silently replace the native recovery schedule with the writer path.
One handle's close must not unlock a
sibling handle. Invalid transitions remain misuse; read-only write requests
remain explicit read-only errors under the existing typed contract.

Use native full-path resolution and temporary-directory discovery, exclusive
randomized temporary creation and `FILE_FLAG_DELETE_ON_CLOSE`. Preserve the
nonempty-file existence rule independently of readability. Honor the shared
`no_follow` contract by opening the final component as a reparse point and
rejecting it, not by following it after a racy pathname-only check.

The randomness front door promises a fully initialized requested buffer,
unlike SQLite's partial-entropy seed callback. Use the OS random-byte service
for this existing Modern contract rather than clone SQLite's clock/PID seed
mixer. This is the same platform-service translation as Unix `getentropy`,
not a replacement SQL PRNG or new hot-path mechanism.

### 3. Keep shared-memory ownership canonical

Translate the reference's process-wide, case-insensitive full-path mapping
owner and per-file attachment. Keep one DMS/mapping handle per owner,
connection-owned shared/exclusive masks and ordinary-volume lock handles.
Preserve the reference's shared-lock-handle accounting for remote volumes.

The first process obtains the exclusive deadman byte, clears stale contents,
then retains its shared deadman lock. Later processes attach without clearing
live contents. Grow the file before mapping, align views to the Windows
allocation granularity, and retain every published borrowed view until the
last attachment releases it. A different region size is misuse.

Unmap releases only the caller's locks. An explicit delete request survives
until the final local attachment, as required by the current `File` contract.
Mapping handles and views remain RAII-owned across failed partial growth.
No second memory-only shared-memory implementation or success-shaped empty
mapping is introduced after an OS error.
The connection retains at most eight exact exclusive OS ranges in fixed
storage so explicit release and RAII detachment can match Windows' original
acquisition ranges. This is bounded native-resource ownership, not another
lock-selection algorithm. Destruction retires every borrowed attachment even
when best-effort OS cleanup fails.

### 4. Qualify native development without weakening existing gates

Use native x64 MSVC/VC++ and Ninja from a Visual Studio developer environment,
with the current strict C++23 warnings-as-errors policy. Record the actual
compiler, SDK, build tools and native Python. The Windows preset selects `cl`
explicitly and owns a separate MSVC build directory so an experimental MinGW
cache cannot silently supply its binaries. Remove the MinGW-specific preset
selection, CI bootstrap and assembler/linker options; do not maintain an
additional Windows GNU support path.
Keep Linux/macOS CI and its sanitizer/fuzz coverage unchanged.
Select the compiler's C++23-only mode explicitly rather than an evolving
`c++latest` dialect. Keep `/W4`, `/permissive-` and `/WX`; use UTF-8 source and
execution encoding, truthful `__cplusplus`, and the supported `/bigobj`
object format for large template units. Do not spoof feature macros, disable
warnings or replace standard ownership containers. Keep project assertions
and the Debug CRT active; the standard-library qualification ABI below is
explicit rather than an implicit promise to qualify vendor instrumentation.
Existing constexpr visitors must put their fallback in the corresponding
compile-time `else` arm, instead of compiling an unreachable trailing return
for already-returning specializations. This is a shared behavior-preserving
control-flow correction, not a Windows-only visitor implementation.
Use standard iterator types rather than assuming a container iterator is a
raw pointer, include referenced standard exceptions directly, and construct
bounded integer values with their declared width. Native test setup reuses
the canonical Windows directory owner and environment APIs instead of relying
on POSIX extensions or disabling CRT deprecation diagnostics.
Native boundary qualification also retains the platform's ordinary stack
reservation. The existing 1,000-height aggregate-attachment control exposed
large recursive binder activations: `BindExpression`, its dispatcher and
`BindIn` occupy 352, 544 and 1,280 bytes respectively in the native Debug
compiler. Move post-child IN finalization out of the recursive driver, as
ADR-0084 already requires for compiler frame ownership. Preserve child order,
normalization, allocation/error behavior and every construction limit; do not
increase the process stack or add a Windows-specific binder.
Apply the same frame separation to the recursive parser entry and its
post-prefix continuation: its unchanged 512-level guard must return the same
typed error on Windows, not exhaust the ordinary stack first. Keep the
canonical fixed-capacity fault VFS's four file images in one setup-owned heap
allocation, before any measured/injected interval. Their capacities, snapshots,
fault positions and all original checks remain unchanged.

Reuse existing unit, model, malformed-input, fault, crash and native-oracle
consumers. Adapt only actual platform assumptions: native VFS selection,
aligned allocation/deallocation pairing, temporary-directory selection and
child-command quoting. Introduce at most one canonical test helper for each
required shared platform operation; preserve allocation injection, counters,
limits, case populations, workload sizes and deadlines.
The existing bounded native trace runner keeps one child/selector/capture
owner. Windows pipe readiness uses `PeekNamedPipe`, because Windows
`select` accepts sockets rather than anonymous process pipes. Preserve the
absolute child deadline, stdout/stderr ceilings, EOF/nonzero-exit controls and
owned-process cleanup on every error. This does not introduce another trace
protocol, oracle or snapshot pipeline.

The POSIX process-group fast-local batch runner remains optional and
POSIX-specific. Windows uses CTest's complete isolated `unit` selection;
this is stronger process isolation, not a smaller fast tier. Do not relax
runner ownership safeguards or silently treat unsupported Python harnesses
as executed. Keep platform-specific qualifications explicit.

The bounded pipe-readiness correction changes the oracle generator fingerprint,
not its corpus or SQLite configuration. Preserve the former profile bytes in
a content-addressed `tests/compatibility/profile-history` snapshot. Historical
archive validation must authenticate that snapshot against its recorded size
and SHA-256 and require every profile field except the generator source
fingerprint to equal the current profile. Generator paths remain identical.
Missing or altered snapshots and configuration changes fail explicitly.
Fresh baseline validation still requires the current byte-exact profile.
Do not rewrite historical measurements, run manifests or raw artifacts, or
run new timing workloads for this platform prerequisite.

Required native evidence includes ordinary and standalone units, local and
child-process database-lock conflicts, close/downgrade cleanup, shared-memory
visibility/lock/stale-state cases, rollback/hot-journal recovery, file temporary
storage, both SQLite interoperability directions and `integrity_check`.
The pinned oracle is compiled for Windows, not loaded from WSL or the system
SQLite. Add a native Windows CI job that actually executes its declared
selection.
Run native clang-tidy on the Windows backend as well as the existing
Linux/macOS production quality selection. Keep the existing automatic
Linux/macOS jobs unchanged. An explicit optional platform-regression dispatch
of the existing Full quality workflow may execute the already required Linux
Debug, instrumentation and four bounded sanitizer/fuzz protocols on hosted
storage. It neither enlarges workloads nor treats the known local WSL
deadline failures as passes.

Hosted Windows qualification discovers the installed x64 Visual Studio C++
tools, initializes their documented developer environment, and uses native
Python, CMake and Ninja without an MSYS2 dependency. Generate the pinned
SQLite amalgamation with its own `Makefile.msc`, `nmake` and bundled Jim Tcl;
authenticate both existing source hashes, then compile the static and shared
oracle libraries with the same MSVC toolchain and frozen SQLite definitions.
Native Jim Tcl needs a normalized script path to derive the pinned source
root for its manifest attestation. Stage only the byte-checked, MSVC-built
source verifier in a dedicated host-tools directory for the generator's
internal lookup. Invoke other generated helpers by explicit paths. Do not
change executable-search safeguards, trust the working directory implicitly,
hand-edit the attestation comment or substitute different source hashes.
The optional Linux fuzz job uses Ubuntu 26.04's Clang 22 toolchain and first
compiles a strict C++23 `std::expected` control; Ubuntu 24.04's default Clang 18
and libstdc++ pair does not provide that required type. Do not spoof language
feature macros or reduce the standard, sanitizer flags or fuzz workloads.

Owned native test children must preserve failure diagnostics. Forward only
explicitly duplicated standard handles through a Win32 handle-inheritance
allowlist; unrelated file, directory and report-lock handles remain excluded.
Keep the existing child deadlines and owned-process termination/reaping rules.

Run the independent read and write fuzz families in separate hosted jobs.
Each family still executes its existing SQL and database campaigns with
20,000 runs, seed 102, original input bounds and the ten-second per-input
deadline. Keep the 25-minute hosted job envelope; do not count a partially
executed sequential batch as qualification or reduce any campaign to fit it.

Execute the native owning platform contracts before building the remaining
Windows test targets. Their focused failure diagnostics include kernel-object
types and an independently initialized C++ synchronization-service baseline;
the file/mapping allocation sweep and exact handle-cleanup assertion remain
unchanged. A focused pass never replaces the subsequent complete native tier.
Earlier MinGW failures remain recorded as failed experiments. In particular,
the cold mapping handle-growth diagnostic does not disappear with the
compiler support correction: the identical strict ownership and allocation
tests must pass under MSVC before native Windows is accepted. Neither this
scope correction nor an MSVC pass waives a failing required Linux sanitizer
gate or proves the cause of an earlier failure.

#### MSVC standard-library qualification ABI

The supported MSVC C++ ABI uses `_ITERATOR_DEBUG_LEVEL=0` together with
`_MSVC_STL_HARDENING=1`. This is one compiler/runtime profile, not a second
engine or a special allocation-test library. Apply the same definitions to
all C++ targets, including GoogleTest, and export them through the existing
public project-options interface. The pinned C oracle's source and frozen
compile definitions remain unchanged.

This admission is required by measured allocation semantics, not timing.
MSVC 19.51's iterator-debug levels 1 and 2 allocate container proxies inside
`noexcept` string default/move and vector default constructors. Independent
controls terminate on injected proxy failure at both levels, while ordinary
sized allocations still throw catchable `std::bad_alloc`. The actual native
open fixture reaches `std::terminate` on allocation prefix 2 through the
`ToWide` string-result move; an observer of that unchanged fixture confirmed
the active `std::bad_alloc`. An outer typed-result catch cannot intercept an
exception that crosses a library `noexcept` boundary. Do not add engine
container replacements or skip these fault prefixes to accommodate intrusive
library bookkeeping.

With the non-intrusive ABI, the same no-throw operations allocate nothing and
real sized allocations still fail normally. Keep `/MDd`, `/Od`, `/RTC1`,
project assertions and all existing workload, allocation, handle and deadline
checks. Enable the vendor's non-intrusive STL hardening explicitly, and add
bootstrap ABI assertions plus owned death-child controls for its documented
span/vector fail-fast checks before invalid memory access. This is not
equivalent to level 2's exhaustive iterator-invalidation tracking; such
instrumented builds are not claimed as qualified allocation-contract builds.
The distinction and public ABI requirement must remain documented.

#### Measured native compiler portability corrections

Retain the ordinary MSVC x64 executable's one-MiB stack reservation. Native
PDB and unwind inspection measured 976/768/384-byte parenthesized-prefix-driver
parser frames, 1,552/1,008/512-byte binary-dispatch-driver binder frames, and
a 20,864-byte recursive joined-loop emitter frame. The unchanged 512 parser,
1,000 expression-construction and 64-source controls expose those costs.
Keep small recursive drivers and move nonrecursive admission/finalization
work out of their live frames. The joined emitter uses one plain borrowed
continuation value between its nonrecursive opening and closing phases;
label creation, instruction order, ON/filter readiness, right replay and
NULL-extension publication remain identical. Do not allocate another
compiler stack or change process stacks, limits or SQL scheduling.

Private constructors that default-construct allocating standard containers
must not promise `noexcept`. The native fault trace reaches
`BtreeWriterCore`'s unordered-map sentinel allocation; let its existing
`OpenWriterCore` allocation catch return typed OOM before coordinator claim.
Parameter-name hashing is similarly lazy: construct its existing map only
for the first named parameter, as pinned `sqlite3ExprAssignVarNumber` and
`sqlite3VListAdd` do, not for parameter-free statements. Preserve numeric and
anonymous assignment, source order and cleanup.

The existing bytecode-builder owner makes its accepted geometric append
growth explicit instead of inheriting vendor-specific vector growth.
The measured MSVC instruction capacities 1/2/3/4/6 add an allocation where
the existing GCC baseline uses 1/2/4/8. Reuse one checked internal growth
mechanism for builder arrays, retain the original exact allocation guards,
and preserve instruction/label limits, owned-byte charging and allocation
failure rollback. This is not permission for per-compiler higher ceilings
or a replacement container.

Native fixture regressions must cover absent, empty and populated temporary
environment values without changing the host environment. Trusted-invariant
death controls verify the explicit termination path rather than a vendor
CRT's undocumented error wording.

## Ordered delivery and review

Keep the scope reviewable under ADR-0106:

1. Review this design and the bounded source mapping before implementation.
2. Deliver the native backend and focused native lifecycle/error/lock tests
   from a recorded red baseline. Preserve the complete Linux fast tier.
3. Deliver canonical session/test/platform integration and native qualification
   through buildable, reviewed increments, with complete native fast units.
4. Close the exact integrated tree with owning native contracts and required
   hosted checks; squash merge and delete only its associated development
   branches. Resume IN query RHS afterward.

Each production increment retains its own red/green or green-refactor
baseline, complete fast-unit result, review and append-only ledger evidence.
An incomplete platform prefix is not described as Windows support.

## Traceability

Node: `implement-advanced-sql`.

Contracts: ADR-0003, ADR-0004, ADR-0005, ADR-0009, ADR-0019, ADR-0020,
ADR-0041, ADR-0042, ADR-0045, ADR-0053, ADR-0102, ADR-0106 and ADR-0111.
