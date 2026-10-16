# ADR-0096: Computed Joined Rowid Probes

- Status: Accepted
- Date: 2026-10-16

## Context

The unchanged correlated read model still exceeds the hosted sanitizer
test's ten-second deadline. The same model timed out at 10.02 seconds on
PR #203 and PR #204. Neither run reports an ASan or UBSan diagnostic.
The separately reviewed journal correction does not reduce this workload.

Exact pinned SQLite source and result-verified growth probes identify an
algorithmic difference:

- `b.id=a.id` admits the existing parameterized rowid lookup;
- `b.id=a.id+?3` instead scans the entire inner source in Modern;
- native `src/wherecode.c:1743-1770` computes either equality key and uses
  `OP_SeekRowid`;
- at 32/64/128 rows, the arithmetic case executes
  18,079/68,575/267,871 Modern VM instructions, versus
  1,300/2,196/3,988 native VM steps; and
- warm probes perform zero source-page reads. The repeated work survives
  page caching.

The complete unchanged model executes 2,034,538 Modern instructions. Its
seeded inputs use the computed form in 46 of 64 iterations per backend.
These opcode totals establish growth, not a cross-engine timing ratio.

ADR-0081 deliberately deferred computed keys because determinism does not
prove evaluation-phase safety. Native probes confirm different constant,
source-dependent, empty-input, and outer-preservation phases for functions
that can return errors. Those general phase differences are not solved by
blindly enabling every deterministic key.

## Decision

### 1. Admit a bounded arithmetic rowid-key domain

Extend joined rowid candidate admission to numeric unary operations
(`+`, `-`, bitwise NOT) and numeric binary operations (`+`, `-`, `*`, `/`,
`%`, shifts, bitwise AND/OR) over the already admitted value leaves and
lazy coalesces. Reuse transparent aliases/collations and existing
determinism/source-readiness proof. Current/later source dependencies remain
ineligible.

These operations use the existing SQL arithmetic implementation: integer
overflow promotes to REAL, division/remainder by zero produces NULL, and
nonintegral/out-of-range rowid keys use the existing no-match conversion.
Do not add a numeric evaluator, key cache, conversion routine, executor, or
bytecode instruction.

Keep function, subquery, aggregate, conditional, concatenation, and logical
key forms outside the new domain. Existing value-only key behavior remains
unchanged.

### 2. Require compatible ON contexts at the probe's source

Admit a newly computed key only when every ON expression scheduled at that
source contains the admitted value/arithmetic forms plus pure comparison,
truth test, likelihood, and logical composition. Use the canonical
`JoinConditionSource` placement calculation, including the source's own ON,
earlier forward-reference ON, and suffix ON placed at an earlier source.
Reject the new candidate if any such expression contains scalar callbacks,
nested queries, aggregates, concatenation, or an unmodeled expression.

The admitted arithmetic/comparison composition has no ordinary SQL-error
callback whose evaluation could be removed by a missing point candidate.
Collation comparison is an existing deterministic `noexcept` contract.
This is a semantic expression-domain rule, not recognition of a particular
SQL string, fixture, model seed, or row count.

Reuse one canonical expression-shape classifier for value, arithmetic-key,
and predicate contexts. Keep the existing predicate visitor and original
ON/WHERE expressions; add no second guard or predicate collection. Preserve
existing value-key preference with the original visitor first; consider
arithmetic keys only when no old value key exists. Both passes retain the
canonical own-ON preference and WHERE preservation barriers. Keep declared
source order and existing rowid-versus-scan costs. Propagate placement errors
through the existing typed optimizer result.

This bounded admission supersedes only the corresponding computed-rowid
deferral in ADR-0081. General callback/error scheduling remains deferred.
Joined stored-index and automatic-index key admission remains value-only
under its existing shared contract; do not broaden those surfaces
accidentally. Nonjoined rowid/index and mutation source-independent keys
retain their existing broader admission and evaluation.

### 3. Reuse the positioned source and preservation state machine

Use the existing generic rowid-key emitter and exact typed seek once per
logical prefix. Retain complete ON and WHERE evaluation after positioning,
at their existing readiness and preservation boundaries.

Keep LEFT/FULL match publication, virtual NULL rows, RIGHT/FULL complete
unmatched replay, all consumers, early/zero LIMIT, reset/rebind, and exact
failure cleanup unchanged. Complete replay ignores the normal computed key,
as it already ignores a value-only point key.

### 4. Prove the original deadline improvement without changing its work

Add a focused physical-plan red and a separate fixed-work regression before
implementation. Do not modify the original model source, fixtures, seed,
64 iterations, two temporary backends, per-iteration preparation, 32 KiB
sorter threshold, ten-second timeout, or existing allocation/work controls.

Repeat the result-verified growth probes, instrument the complete unchanged
model, and use alternating old/new complete-model runs before claiming
runtime improvement. Local results do not substitute for the exact-head
hosted sanitizer gate. Do not request a blind rerun.

## Verification

Cover all join kinds, normal/missing/NULL/fractional/out-of-range keys,
arithmetic boundaries, transparent aliases, coalesced virtual prefixes,
forward/current dependencies, WHERE/outer barriers, and unchanged
function/volatile/unsafe-ON fallbacks. Include earlier forward and suffix ON
expressions scheduled at the probed source, not just its own ON.

Exercise covering, table-backed stored-index, and automatic-index fallbacks
without broadening their key domain. Pin unchanged source-independent
nonjoined rowid/index and mutation keys. Cover ordered/grouped/global/private
scalar/EXISTS/set consumers, reset/rebind, zero/early LIMIT, malformed input,
allocation and source-I/O cleanup, and complete RIGHT/FULL replay.

Retain independent native vectors and the 27 inherited source-correlation
work properties. Re-run the exact 80-case native phase comparison: preserve
all 64 matching outcomes and introduce no mismatch beyond the 16 recorded
pre-existing differences. Run the complete fast tier and required compiler,
sanitizer, native-contract, generated-input, format, tidy, engineering, and
graph checks. Obtain GPT-5.6 Sol design/implementation/exact-tree review.
Only the unchanged model passing the exact-head hosted gate closes the
original timeout correction.

### Focused and measured evidence

The original physical-plan test first observed the missing lookup. The
unchanged five-column FULL query returned all nine expected rows but
executed 18,079 instructions, failing its new 4,096-instruction bound before
production changes.

GPT-5.6 Sol rejected an initial own-ON-only gate: earlier forward or suffix
ON expressions can also be scheduled at the probed source. New plan and
execution regressions demonstrated that omission. The revised design and
bounded implementation, using canonical `JoinConditionSource` across all
same-core joins, were accepted with no remaining blockers.

The unchanged complete model now executes 587,276 instructions instead of
2,034,538, a 71.1% reduction. Result-verified arithmetic growth probes at
32/64/128 rows execute 2,343/4,359/8,391 instructions instead of
18,079/68,575/267,871, with zero warm source-page reads. Their results still
match pinned SQLite, and the exact 80-case phase gate retains 64 matches and
the same 16 pre-existing differences, with no new mismatch.

Five alternating complete-model pairs after symmetric warmup measure:

| Configuration | Baseline median CPU seconds | Candidate median CPU seconds | Median paired ratio |
|---|---:|---:|---:|
| Apple Clang ASan/UBSan | 4.5751 | 2.2091 | 0.4840 |
| GNU 16 Debug | 1.2869 | 0.6831 | 0.5281 |
| Release | 0.0622 | 0.0359 | 0.5775 |

The measurements retain the original model source, seed, 64 iterations, both
backends, per-iteration preparation, and threshold. They are local evidence,
not a claim that the hosted deadline is already closed. A 2,511-case
cross-feature native comparison also gains no new difference.

### Local gate closure

Complete Debug, Release, and ASan/UBSan tiers each pass 1,455 cases;
instrumentation passes 1,468, and GNU 16 Debug/Release each pass 1,452.
Two existing GNU Debug cases crossed local maintenance sleeps of 582 and
927 seconds. Their ten-second limits were unchanged; only those two cases
were rechecked after diagnosis, passing in 0.23 and 0.15 seconds. All other
GNU Debug cases passed on the initial run.

All 68 isolated native contracts pass, and the 27 inherited work properties
remain identical. The unchanged sanitized read-input campaign completes
20,000 inputs with seed 91. Four new pinned native correlated-join cases
preserve all 198 inherited subquery corpus/oracle records. Numeric key forms,
all join kinds, callback/placement barriers, both backends, twelve rebound
offset cases, and complete optimizer/lowering allocation sweeps pass.

Formatting, changed-translation-unit clang-tidy, engineering discipline,
graph validation, and whitespace checks pass. No original model, fixture,
test deadline, workflow, workload, or resource ceiling changed. Exact-tree
review and the exact published head's hosted checks remain required before
merge or deadline closure.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 join access/scaling prerequisite.

Contracts: ADR-0035, ADR-0036, ADR-0052, ADR-0053, ADR-0077, ADR-0079,
ADR-0080, ADR-0081, ADR-0087, ADR-0089, and ADR-0090.

PR #204, the separate audit corrections, and later feature expansion remain
paused while this original deadline correction is active.
