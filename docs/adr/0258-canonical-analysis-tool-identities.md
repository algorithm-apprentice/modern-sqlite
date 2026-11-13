# ADR-0258: Canonical Analysis Tool Identities

- Status: Accepted
- Date: 2026-11-13
- Module graph node: `implement-advanced-sql`
- Blocked main: PR #332 `3166e15e`, tree `14a5bb1c`, ledger396
- Qualified ancestor: PR #331 `9be30993`, ledger394
- Related contracts: ADR-0102/0106/0236/0239/0243/0256/0257

## Evidence and scope

PR #332's accepted SELECT correction passes its complete selected local plan
and original premerge CI. Original actual-main CI43366640560 nevertheless
times out two unchanged tooling owners: native Windows
`tools.ci_workflow_policy` at10.03s and Mac Release
`tools.changed_clang_tidy_dependency` at10.06s, each under10s. Their failed
records, unexecuted suffixes and retained source references remain authoritative.

Automatic Full quality43366640341 separately exceeds its45-minute job limit.
The job record reaches the clang-tidy build, but its log archive is empty.
This identifies neither a translation unit nor a compiler/provider cause.
No successful qualification or source-neutral request is inferred.

Local unchanged-owner controls pass, but use different host profiles. Native
policy profiling retains32 methods and all twelve real CMake configuration/
discovery fixtures. Repeated repository text reads contribute only about0.03s
in that diagnostic, so a new workflow-text cache is not justified.

The dependency owner exposes one concrete duplicate operation. Its ten real
analysis executions take twenty before/after contexts. Each context separately
hashes the compiler and analyzer even when both roles resolve to the same
Python executable. The local read observation records forty reads totaling
320,837,120 bytes for that image; one read per context would retain twenty
reads and remove160,418,560 repeated bytes. This is a fixed-work observation,
not matching macOS timing or an explanation of either hosted timeout.

## Existing owners

| Responsibility | Canonical owner |
|---|---|
| Resolve and require each configured tool role | `analysis_context` |
| Hash each resolved command file once per snapshot | `verify.command_file_identities` |
| Validate source/configuration/command and dependency closure | `analysis_context`, `dependency_snapshot` |
| Compare fresh execution and reuse inputs before/after | `run_reported_analysis` |
| Preserve original execution and explicit failures | ADR-0256 report owners |

The shared command-file helper already deduplicates resolved paths within one
call. The analyzer's separate per-role digest loop duplicates that mechanism.
No cross-invocation cache or new fingerprint implementation is needed.

## Decision

Resolve and validate all four existing roles as before: Ninja, compiler,
analyzer and CMake. Pass their resolved executable paths together to the
existing `verify.command_file_identities` helper. Build the unchanged per-role
`path`/`sha256` records from that one snapshot.

Roles remain distinct. Only equal resolved paths share a byte read; distinct
paths with identical content remain distinct identities. Do not coalesce
hardlinks by inode or infer equality from a filename or digest.

Every required role must have an identity. If a tool disappears between
resolution and fingerprinting, fail explicitly through `VerificationError`
before launching analysis. A partial helper result must not produce an
omitted role, a default digest or an unhandled dictionary lookup.

Take a new snapshot at every existing before/after boundary, including reuse
reconciliation. Do not retain hashes between snapshots, methods or commands.
Keep source, environment, configuration, selected commands, dependency modes,
compiler/header bytes, object checks and original execution receipts intact.
Tool or source changes must still prevent reuse or a passed execution.

Do not change the canonical helper's other callers, report schema, platform
admission, subprocess ownership, analyzer count, fixture population, CTest
names/properties, deadlines, job budgets or workflow settings. In particular,
the native policy and full-production quality failures are not claimed fixed
by this bounded correction.

## Verification and delivery

Use the existing reported-analysis fixture. Add a fixed-work assertion to an
existing real-analysis control: aliased compiler/analyzer roles read the image
once before and once after, not four times, while still running the analyzer.
Record the current count failure before changing production code. Cover a
tool disappearing during snapshot admission and retain all reuse, dependency,
launch-failure and source-drift controls.

Compare the same diagnostic selection after the change, distinguishing byte
counts from elapsed time and retaining profiling overhead. Run focused tests,
the complete owning tool tier and actual configured owners under unchanged
deadlines. Do not claim a historical latency cause from a later passing run.

Review design before implementation and the exact implementation tree before
commit. Deliver this coherent tooling correction separately, preserving all
four stashes and the blocked SELECT source references. Reconcile unchanged
engine evidence against the qualified ancestor and apply the actual closing
union. Fresh required exact-head and descendant-main execution, not local
diagnostics or a plan, is necessary to close ledger396's delivery block.

No prior one-shot diagnostic allowance is reopened. General source
preparation, `NC_NoSelect`, IN and node43 remain incomplete.
