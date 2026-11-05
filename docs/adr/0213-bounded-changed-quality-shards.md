# ADR-0213: Bounded Changed-Quality Shards

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source226 `4266349`, ledger226, draft PR #294
- Related contracts: ADR-0009, ADR-0102/0106 and ADR-0211/0212

## Observed remaining gate

Source226 passes the other eight standard-CI contexts. Actual Intel
Debug/Release selections complete after the canonical fixture correction.
CI41924649499 still fails because the changed-quality job exceeds its
unchanged ten-minute job deadline. Its annotation explicitly records that
limit; formatting does not execute after cancellation.

The same changed-quality cancellation is present in CI41903485476 and
CI41918653114. Those full raw records were retained, but earlier focused
failure-only summaries omitted the cancelled context. It is not successful
evidence. Source223's earlier successful context checked only the ordinary
smoke unit; source224 introduces the seven changed production units, giving
eight units including the smoke source.

The independent full-production quality jobs execute successfully with the
same declared Intel/LLVM22 profile. Source225's full quality build takes
12 minutes52 seconds; this does not waive the cancelled standard context.
The selected local LLVM22/libc++ inventory also records substantial costs
for binder, lowerer and optimizer analysis. Local timings are not matched
Apple-SDK timings, and no unique CPU, memory or setup cause is established.

The current selector already owns the canonical set: changed compiled
production sources plus an optional smoke source. It invokes CMake's exact
object targets, retaining both configured clang-tidy and compiler work.
Do not remove compilation, replace the analyzer command, filter diagnostics,
change checks or raise the job deadline to obtain a pass.

## Decision: partition whole selected units

Extend the existing selector with one optional shard index,0 through2.
Without it, preserve the existing unsharded selection and invocation.
Sharded mode uses exactly three partitions and one CMake build process
per worker. Reject invalid indices or a conflicting parallel-job request.
There is no new tool, source parser, compiler wrapper or analyzer runner.
Resolve relative compilation-database file/output paths against their
entry's working directory, not the caller's directory. This preserves the
same target identity for current relative and absolute provider formats.

One pure partition function assigns every selected source exactly once.
Sort sources by descending byte size with canonical path as the tie-breaker;
assign each to the currently smallest accumulated byte total, breaking
ties by shard index. A zero-byte file has weight one. Sort each resulting
target list for stable invocation. File size is a deterministic scheduling
weight, not a timing estimate or a performance claim.

The current eight-source set separates into a lowerer partition, a
binder/inspector/function-registry partition, and an optimizer/VM/transaction/
smoke partition. No translation unit is split or omitted. Header, malformed,
allocation and compiler diagnostics remain exactly the configured checks.
Empty assignments report no work rather than claiming analyzer execution.

Emit one bounded, structured selection record identifying the complete
selected set, assigned set, object targets, shard index/count and actual
parallelism. Final authentication verifies identical complete selections,
disjoint assignments, complete coverage and successful original CMake
commands. A successful context without this execution evidence is insufficient.

## Hosted ownership and resource contract

Move the existing macOS quality body into `quality_shards`. Pull requests
create three `macos-26-intel` workers; push events create only shard0, since
changed-source analysis remains PR-only. Every worker keeps the original
ten-minute job deadline, verified CMake4.4.5 bootstrap, LLVM/formatter and
action pins. Each analysis command uses `--jobs 1`; matrix concurrency is
bounded at three. Thus at most three analysis/compilation processes run,
as before, but they receive separate standard hosted memory/address spaces.
The additional hosted workers and potentially different total workflow
latency are explicit; this is not equivalence to the former whole-job clock.
Do not adopt paid larger runners or change repository protection settings.

Configuration and required tool setup remain per executing worker.
Engineering discipline and formatting have one owner: shard2 on PRs and
shard0 on push. Preserve their original commands and ordering around
configuration/analysis. Other workers do not repeat these shared checks.
The smoke source remains part of the one canonical partitioned selection.

Retain the legacy `quality` context as an always-evaluated result gate
depending on the entire worker matrix. Only a successful matrix may produce
a successful gate; failure, cancellation, skipping or absent results fail it.
The other eight legacy contexts and their workloads do not change.

This narrowly supersedes the previous numeric nine-context assumption:
PR standard CI now requires twelve contexts (nine legacy roles plus three
quality workers); actual main requires ten (nine roles plus its one worker).
All applicable worker bodies and the gate are mandatory. The separate
four-job Full quality/platform/fuzz union remains unchanged and mandatory
for this delivery. Original intentional PR nonexecution is not execution
credit, and main's changed-analysis nonexecution policy remains explicit.

## Test-first and delivery

Add focused tests before implementation for deterministic disjoint complete
partitioning, skewed/empty inputs, invalid shard requests and exact assigned
CMake target/parallel arguments. Keep the existing production-source scope
and MSVC compilation-database controls. Workflow-policy tests cover matrix
cardinality, the single shared-check owner, aggregate failure propagation,
all unchanged profiles/pins/deadlines and preservation of the eight other
legacy roles. Exercise the extracted result gate for every non-success state.

Run focused red/green and complete owning tool selections. Use the actual
source/batch verification plans and retain original compiled/runtime/fuzz
execution when all relevant inputs and identities match. No fresh macOS
timing or deadline success is inferred from static partitioning or local
selection tests; the exact new hosted head must execute every worker.

Admit one bounded quality-gate repair source in the existing PR #294 under
ADR-0106's final-gate repair discipline. This named delivery exception retains
all accepted history and per-source evidence; it does not admit another SQL
feature or create a general batch-size waiver. Independent design, code and
source review precede the normal commit and ledger append. Preserve every
failed/cancelled run, then require the new twelve-plus-four PR union,
integrated review, normal squash and actual-main ten-context qualification
before branch retirement. Further failures remain failures, not permission
to relax limits. General SQL prerequisites, IN and node43 remain incomplete.

## References

- [GitHub job matrices](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/run-job-variations)
- [GitHub dependency result contexts](https://docs.github.com/en/actions/reference/workflows-and-actions/contexts#needs-context)
- [CMake target clang-tidy ownership](https://cmake.org/cmake/help/latest/prop_tgt/LANG_CLANG_TIDY.html)
