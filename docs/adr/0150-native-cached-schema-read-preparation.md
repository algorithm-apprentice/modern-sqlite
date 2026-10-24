# ADR-0150: Native Cached-Schema Read Preparation

- Status: Accepted
- Date: 2026-10-25
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 15

## Completed source and work attribution

PR #255 closes parser entry 14 and publishes the independent phase baseline.
Short complete preparation remains 15.643751x native: 7.293 us Modern versus
0.466 us native. Separately measured pure parsing is 0.696 us; those lifecycle
measurements are not an additive call-site profile.

The qualified diagnostic performs seven VFS calls per successful Modern
prepare, with no page reads after warmup. Ordinary, equal-work
10,000,000-operation replays of the same short SQL have identical work digests.
Passive Modern samples prominently reach `BeginRead`, lock/unlock, file
metadata and header reads; native samples instead reach parser, resolution
and code-generation work. Sample counts are attribution, not timing evidence.

The source difference is concrete:

| Native owner and transition | Pinned source |
|---|---|
| Schema initialization skips databases already marked loaded | `prepare.c:453-479` |
| `sqlite3ReadSchema` retains that initialized connection schema | `prepare.c:490-503` |
| Missing table and missing/ambiguous column request a schema check | `build.c:409-454`; `resolve.c:782-798` |
| Speculative compound ordering suppresses diagnostics, not the schema request | `resolve.c:1564-1590` |
| Missing qualified wildcard/table-list diagnostics do not request that probe | `select.c:6414-6418` |
| Requested check temporarily opens a read transaction, compares cookies and closes it | `prepare.c:512-553` |
| Known schema invalidation clears loaded state before the next load | `build.c:627-644` |
| Non-OOM probe failure preserves the original semantic error; OOM takes precedence | `prepare.c:529-535,818-828` |
| A schema mismatch resets/reloads and retries preparation once | `prepare.c:864-900` |
| Statement execution checks schema and automatically reprepares | `vdbeapi.c:980-1120` |

Modern `ReadSession::Prepare` instead begins and ends a temporary read on
every substantive prepare, even with an already owned immutable catalog.
The existing execution path already acquires the actual read snapshot,
refreshes the catalog and recompiles on generation/cookie change before
running bytecode.

A deterministic VFS probe against the unchanged pinned public C API confirms:

- warmed valid prepare makes zero read/size/lock/unlock/access calls even
  with read failure armed;
- the subsequent step reports `SQLITE_IOERR`, after actual read admission;
- an unknown function reports `SQLITE_ERROR` without checking the file; and
- a missing-table schema check attempts the read but preserves its original
  `SQLITE_ERROR` when that probe encounters ordinary I/O failure.

## Decision

### 1. Retain one connection catalog, not a statement cache

For the read facade, compile against the existing immutable catalog when it
is already initialized. Do not begin a temporary read transaction merely to
compile a valid statement against that owner.

Keep the existing cleanup barrier before every substantive prepare. Its
canonical Pager operation is already a no-op with no retained lock; pending
failed begin/end cleanup must still complete or fail explicitly before
compilation. Preserve parsing-before-storage and empty-input behavior.

The first catalog load retains the current temporary snapshot, publication,
failure cleanup and generation rules. A prepare performed while another
statement is suspended uses that statement's existing catalog/snapshot.
Keep one private initialized/needs-reload state corresponding to native
`DB_SchemaLoaded`. This is distinct from the retained immutable owner: a known
stale snapshot may remain alive for existing views after a failed replacement,
but must not be treated as initialized by the next prepare. Successful
publication restores initialized state; known invalidation clears it before
replacement. No second catalog, generation token or statement cache is added.

### 2. Reuse the existing compiler and one bounded schema retry

Record one statement-local sticky schema-check request at the canonical
native-equivalent table/column resolver failure sites. It is compilation
diagnostic state, not query-local arena state: speculative checkpoints,
discarded expression attempts and nested query restoration must not clear it.
Publish it only when binding ultimately fails, as one typed fact on the
existing `BindError`. Preserve the session's existing typed `Error` and public
API; never infer this state from rendered diagnostics.

A final error-code switch is insufficient. A speculative missing-column
lookup can be suppressed and replaced with `kCompoundOrderByTerm`; native
retains `checkSchema` in that path. Conversely, a missing qualified wildcard
uses the same `kNoSuchTable` code without native schema-check provenance.
Unknown functions, arity errors, out-of-range ordering, malformed syntax and
other unmarked failures must not acquire a probe. Do not blanket-enable
checks for every compound-order or name-shaped final error.

When requested, use the existing Pager to probe the cookie. If unchanged,
close any probe-owned transaction and return the original semantic error.
If changed, invalidate initialized state and close that probe transaction
before the retry. Parse the retained SQL, enter the existing cold catalog-load
path and invoke the same compiler again, once. An already active statement
snapshot is borrowed, never ended by preparation. Preserve parameters,
source/tail ownership and publication rules; no AST/IR clone or alternate
resolver is added.

OOM from validation takes precedence. Ordinary validation I/O/busy failure
keeps the original primary semantic error, with the repository's explicit
contextual error combination rather than silently discarding the secondary
failure. A failure loading a known replacement on the subsequent cold retry
instead returns that load error. Keep the old snapshot owner but leave
initialized state clear, so later successful-looking names cannot bypass the
failed replacement. Every probe/retry-owned transaction must be ended, and
failed cleanup remains subject to the existing barrier.

### 3. Preserve execution-time admission and metadata lifetime

Storage-reading statements still enter the read snapshot at first step,
refresh the catalog and run the current automatic reprepare path. Valid warm
preparation may therefore expose cached result metadata until first step,
as native preparation does. Newly introduced names still receive the
bounded failure-triggered reload above.

Binding persistence, row/metadata view lifetimes, stable execution owners,
reset/finalize errors, suspended statements, scalar snapshot-free execution
and all existing cleanup barriers remain unchanged.

This supersedes only ADR-0037's unconditional temporary read/catalog refresh
for an already initialized successful read prepare. Its cold validation,
snapshot-entry, generation and ownership contracts remain canonical.

### 4. Keep the unresolved scope explicit

This is one bounded read-facade repair supported by measured work. The write
facade has a separate transaction coordinator, pending catalog publication,
rollback and savepoint ownership; it is not changed by assuming those states
are equivalent. Its corresponding preparation policy requires its own
completed caller/rollback map before any follow-up change.

Ledger 15 is not automatically closed by removing these seven calls. After
qualification, repeat the matched phase evidence and attribute remaining
wide/deep/repeated-copy work through the current binder and compiler owners.
Do not start later SQL or DAG nodes while that ledger remains unresolved.

## Focused verification

Review this decision before production implementation. First add a public
read-session regression for zero VFS calls on warm successful prepare with
the read failure deferred to step; record the failure on unchanged production.

Owning tests also cover cold corrupt/empty images, missing/ambiguous names,
ordinary function errors, unchanged-cookie probe errors, changed-cookie
reload/retry, failure after known invalidation, wildcard metadata before/after
first step, a stale-schema compound wildcard ordered by a newly added column,
negative compound-ordinal and qualified-wildcard controls, active snapshots,
retained cleanup, OOM, reset/finalize and binding preservation. Reuse current
VFS/fault helpers and fixtures. Preserve the native primary-error distinction.

Run the complete fast units and actual change-aware local/hosted gates.
Regenerate matched short/wide/deep/malformed phase evidence from clean reviewed
source with equal work, unchanged ordinary/diagnostic separation and current
native identity. No source limit, allocation ceiling, threshold or model
deadline is weakened.

## Qualified read-facade result

PR #256 qualifies source `3658b0e`, tree
`47639f6a80d135d6245efa1d2643f1c7004a67de`.
The [portable phase archive](../../benchmarks/cached-schema-preparation-v1.json)
contains all 96 ordinary timing children and 32 separate diagnostic children,
with nine samples per engine/phase and the unchanged native public prepare.
Fresh generation and current-source validation passed before archiving; the
historical validator reproduces the recorded work, lifecycle and profile
checks without requiring the original machine. The owning CTest entry uses
that same validator and joins configuration-independent reconciliation.

| Successful prepare shape | Modern us/op | Native us/op | Wall ratio |
|---|---:|---:|---:|
| Short | 3.763 | 0.472 | 7.966699x |
| 128 columns | 36.268 | 19.961 | 1.816940x |
| 512 columns | 131.448 | 84.958 | 1.547211x |
| 64 parentheses | 8.805 | 1.445 | 6.095600x |
| 256 parentheses | 25.823 | 4.056 | 6.366916x |

All five successful warm shapes now make zero Modern VFS calls. Short
preparation uses 81 allocations per operation, versus 83 historically.
Under identical recorded workload, build and host profiles, the historical
short Modern median is 7.293 us versus the fresh 3.763 us; the corresponding
native medians are 0.466 us and 0.472 us. These are historical versus fresh
matched runs, not simultaneous before/after samples. Pure syntax and
malformed shapes are retained in the archive, not omitted from qualification.

Subsequent ordinary equal-work passive samples cover short (10,000,000
operations per engine), wide-512 (100,000) and deep-256 (1,000,000).
Modern short/wide leaves now prominently include allocation and program
analysis rather than the removed file-admission path. Deep samples include
the source-preserving parser and recursive parenthesized-expression binding;
native prominently consumes tokens/parser reductions. Samples do not assign
additive phase costs, quantify a speedup or prove a copy's ownership purpose.
Compiler/binder ownership and replay disposition, plus write-coordinator
preparation, remain entry 15's explicit follow-up.

Nine focused binder/session contracts and complete fast 1,777+97 pass.
The actual closing plan passes all six isolated configurations (1,916 tests
each, 1,937 instrumented), 30 shared checks/182 reconciled instances,
59 native contracts, four 20,000-run campaigns and production quality.
The original assertions, allocation limits and model deadlines are unchanged.
Exact final-head hosted acceptance remains a delivery gate.
