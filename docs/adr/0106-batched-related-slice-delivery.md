# ADR-0106: Batched Related Slice Delivery

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

`implement-advanced-sql` is intentionally oversized. It includes joins,
sorting, aggregates, nested queries, CTEs, views, constraints, triggers, and
reference-alignment prerequisites. Completing one pull request does not
complete the node.

The existing one-slice-per-pull-request practice kept each correction easy to
review, but it repeats a substantial fixed delivery cost:

- design, implementation, and exact-tree reviews;
- branch publication and pull-request administration;
- nine hosted checks;
- guarded squash verification; and
- named branch cleanup.

ADR-0102 reduced local repetition, but required hosted checks still run once
per pull-request head. For node 43, very small pull requests now spend more
time on repeated delivery mechanics than on the bounded implementation.

The user requested a larger delivery unit: each slice should remain a distinct
commit, while several slices share one pull request. The user then explicitly
approved one pull request for all remaining node-43 prerequisite corrections,
while keeping later feature delivery separate.

## Decision

### 0. Supersession scope

This ADR supersedes only pull-request granularity and repeated per-PR closing
gates in:

- ADR-0046 section 4 items 1–2;
- ADR-0053 section 1's sequential-pull-request wording and item 7;
- ADR-0092's requirement that expression copies and aggregate ordering use
  separate pull requests with separate closing matrices;
- `AGENTS.md`'s one-coherent-slice-per-PR sentence; and
- `docs/dependency-dag.md`'s equivalent one-slice-per-PR rule.

It preserves their slice order, architectural boundaries, accepted-predecessor
rule, per-slice documentation/TDD/review requirements, active-node completion,
and later-node blocking. No later slice may begin until its predecessor commit
is accepted under this ADR.

### 1. Add a related-slice delivery batch

A normal delivery batch contains two through four ordered commits in one pull
request. The node-43 prerequisite-closure batch is an explicit exception: it
contains the seven remaining prerequisite corrections in one pull request.
Each commit is one coherent slice and must independently have:

- an accepted design or an existing accepted contract;
- focused red/green evidence, or a green refactor baseline;
- complete fast-unit evidence;
- direct implementation review; and
- explicit traceability to the active module-graph node and relevant ADRs.

Later commits in the batch may depend only on earlier commits in the same
linear batch. A commit must leave the branch buildable and its own focused
tests green. Reviewers can inspect each commit range separately.

Before the next slice begins, append one JSON-lines record to the batch ledger
at the session evidence path
`node43-prerequisite-closure-ledger.jsonl`. Each slice record contains:

- slice ordinal/name and immediate predecessor;
- reviewed tree identity, resulting commit SHA, and proof that the commit owns
  that reviewed tree;
- accepted ADR/contract and design-review reference;
- focused-red source identity, command, and expected failure signature, or a
  green refactor baseline;
- focused-green and complete-fast report identities/results;
- applicable malformed, boundary, allocation/OOM, ownership, lifecycle, and
  failure-injection selections/results;
- implementation-review reference and Accepted verdict tied to the reviewed
  tree;
- changed paths and tests; and
- node/ADR traceability.

Ledger records are append-only. Remediation or evidence-only commits receive
their own records and do not count as production slices.

### 2. Batch only tightly related mechanisms

Slices may share a pull request when they:

- belong to the same active DAG node;
- modify the same architectural layer or one direct producer/consumer chain;
- require substantially the same compiler, sanitizer, differential, fuzz,
  native, and performance gates; and
- produce a coherent integrated behavior that is still reviewable as one pull
  request.

Do not normally batch unrelated SQL semantics, persistent-format changes,
durability state transitions, public API work, extension work, or later DAG
nodes merely to reduce CI runs. The prerequisite-closure exception is bounded
to already confirmed node-43 findings and already ordered ADR-0092 follow-ups.
It adds no later feature slice.

Outside that explicit exception, if a planned batch grows beyond four slices,
crosses another layer, or cannot be reviewed commit by commit, split it into
ordered stacked pull requests.

Stop before beginning the next slice when:

- design or implementation review is not Accepted;
- focused or fast evidence is not green and source-matched;
- the commit is not buildable; or
- its ledger record is incomplete.

Stop and split the seven-slice exception when a finding:

- expands beyond its named correction;
- adds an eighth production slice;
- enters IN, FROM/coroutine, CTE, views, constraints, triggers, windows, or a
  later DAG node;
- requires an unplanned architectural mechanism;
- introduces a new durable state transition, public API, or temporary format;
- creates a non-linear dependency; or
- cannot be reviewed as an isolated commit range.

The REAL/IntReal slice remains in this batch only as the already identified
serial-form compatibility correction. A broader format redesign or new
persistent transition requires its own child ADR and pull request.

### 3. Keep commit-level development evidence

Documentation-first and test-first development remain per slice:

1. review the slice design;
2. record its focused failure;
3. implement the smallest correction;
4. run focused green and the complete fast tier;
5. review that commit's implementation; and
6. commit the slice before beginning the next one.

Do not defer malformed-input, allocation, ownership, lifecycle, or
failure-injection tests to the batch end. Do not hide an incomplete earlier
slice behind a later commit.

### 4. Run integrated delivery gates once per final batch head

After every commit-level review is accepted:

- run ADR-0102's full verification profile against the complete base-to-head
  diff for the seven-slice prerequisite exception;
- reconcile configuration-independent checks before exclusion;
- run every applicable isolated compiler/sanitizer/model configuration,
  pinned native compatibility, SQL/database fuzz domain selected by the
  combined SQL/VM/codec changes, persistent serial-form golden/
  interoperability/failure evidence, and matched performance contract once
  against the final exact tree;
- obtain one integrated exact-tree review covering all commit identities and
  their combined behavior; and
- require all nine exact-head hosted checks before guarded squash merge.

Normal two-to-four-slice batches continue to use ADR-0102's change-aware
selected union. Any follow-up commit creates a new final head and invalidates
prior exact-head review and hosted success until the complete required
final-head gates pass again.

A failed final gate is corrected with a normal follow-up commit. Do not amend,
force-push, bypass, or blindly rerun. If the failure reveals that a commit is
not independently coherent, split or reorder the batch even after draft
publication.

A localized final-gate repair is linked to its owning slice and repeats that
slice's focused/fast review before final gates restart. If a repair changes an
earlier slice's contract or source consumed by later commits, every dependent
suffix evidence record and implementation review is invalid. Re-run and
re-review that suffix before closure.

Published history is never rewritten. If append-only repair/revert commits
make commit-range review unclear, close the draft as superseded and publish a
clean replacement or split at the last accepted prefix. Reverting an ancestor
also reverts or invalidates its dependent suffix.

The pull request may squash on merge, but commit identities, focused evidence,
reviews, and the pull-request history preserve the slice boundaries.

### 5. Close all remaining prerequisites in one pull request

The prerequisite-closure branch contains these ordered commits:

1. rowid range access;
2. joined ON/access evaluation phase correction;
3. REAL/IntReal persistent serial-form correction;
4. virtual NULL-row/range verifier-state correction;
5. ADR-0092 semantic query-expression copies;
6. ADR-0092 query-bearing aggregate ORDER registration; and
7. outer aggregate ownership.

Commit-level design, red/green evidence, fast units, and implementation review
remain mandatory before beginning the next commit. Do not publish the pull
request as ready for merge until all seven commits are complete and the
integrated exact tree is reviewed.

IN, FROM/coroutine, CTE, view, constraint, trigger, window, and other feature
slices remain outside this prerequisite pull request and use later related
batches.

This policy changes delivery granularity only. It does not relax node
completion, architecture, SQLite compatibility, review, testing, or hosted
gate requirements.

The final ledger adds:

- batch base and final exact head;
- every production and remediation commit/range;
- complete verification-plan union and shared-check reconciliation;
- isolated/native/fuzz/persistent-format/performance evidence;
- integrated exact-tree review; and
- all nine hosted run identities.

Before squash merge, publish the complete ledger or an equivalent rendered
record in the pull-request discussion and have the final acceptance review
reference that durable URL. The local JSONL digest alone is insufficient.

## Consequences

- Hosted and administrative fixed costs are paid once for the complete
  prerequisite closure and once per later related batch.
- Review remains bounded because each slice has a stable commit range.
- Integrated failures are discovered before merge rather than after multiple
  independently merged prerequisites.
- A batch branch lives longer than a single-slice branch, so commit discipline
  and source/evidence identity become more important.
- Changing an accepted ancestor invalidates every dependent suffix until it is
  re-reviewed and requalified.
- High-risk or unrelated work still uses separate pull requests.

## References

- ADR-0005: Test-Driven Development
- ADR-0006: AI-Native Sequential Workflow
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0092: Semantic Query Instances for Result References
- ADR-0102: Change-Aware Verification
