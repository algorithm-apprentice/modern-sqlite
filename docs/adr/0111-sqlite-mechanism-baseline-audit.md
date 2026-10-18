# ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`

## Context

Modern SQLite has completed nodes 1 through 42 and several ordered slices of
node 43. The engine already preserves the accepted C++23 ownership model,
acyclic dependency graph, SQLite database format, bidirectional
interoperability, observable SQL compatibility, and matched performance
harnesses.

ADR-0045 deliberately assigns most SQL-facing modules to observable-semantic
alignment rather than source-architecture reproduction. That distinction is
necessary: copying SQLite's mutable parser, planner/VDBE coupling, master
internal header, or concrete cross-layer structures would violate ADR-0003.

Observable parity alone is nevertheless too weak for a first implementation
of a performance-sensitive or stateful mechanism. A custom algorithm can pass
the current corpus while retaining:

- worse asymptotic work outside the tested sizes;
- avoidable allocation, copying, validation, or virtual dispatch;
- different evaluation, ownership, or cleanup phases;
- failure-state differences visible only under injection;
- planner choices that preserve rows but lose SQLite's proven access path; or
- a locally convenient mechanism that later features cannot safely reuse.

The existing implemented-engine audit found examples in every risk class:
custom B-tree mutation, nullable range traversal, joined access phases,
quadratic joined probes, REAL/IntReal record formation, journal metadata
recovery, statement completion, and aggregate ownership. These corrections
were successful, but the audit explicitly did not claim module-wide
equivalence. Several advanced-SQL mechanisms were also implemented before
their complete SQLite source mechanism had been frozen, causing review to
discover missing cases after implementation.

The project owner therefore requires one broad implemented-engine review
before feature expansion continues: use SQLite's proven mechanism as the
first baseline, identify every implemented mechanism that materially differs,
repair those differences, and optimize or refactor only after parity and a
matched baseline exist.

The local pinned SQLite 3.54.0 source identified by `project/manifest.json` is
the only source oracle. Web or mirror copies are not substitutes when that
checkout is available.

## Decision

### 1. Add a mechanism-baseline requirement to ADR-0045

ADR-0045's three alignment levels remain, but Level B and performance-relevant
Level C code gain an additional first-version rule.

For an implemented mechanism that controls any of the following:

- algorithmic complexity or database-size-dependent work;
- evaluation order, callback frequency, or error phase;
- state transitions, cleanup, retry, or statement lifecycle;
- ownership, pinning, borrowing, or scratch lifetime;
- record, key, row, page, or temporary-data movement;
- planner access choice or runtime cursor schedule; or
- allocation, I/O, comparison, copying, or VM-instruction volume,

the first compatible implementation must translate SQLite's proven mechanism
unless an accepted ADR already authorizes a different mechanism with matched
evidence.

Translation preserves the native algorithm, state machine, ordering,
boundedness, ownership intent, and failure cleanup. It does not require the
same source files, structs, opcodes, pointer representation, mutable AST, or
dependency cycles. SQLite responsibilities may be split across Modern
modules, but their semantic and performance-relevant invariants must survive
at the seams.

A different mechanism is acceptable only when an ADR records:

1. the exact native mechanism and prerequisites;
2. why it is incompatible with the accepted Modern architecture or current
   requirement;
3. matched correctness and lifecycle evidence;
4. matched timing and fixed-work evidence for every affected hot path;
5. allocation, failure, cleanup, and non-target regression evidence; and
6. the scope in which the alternative is canonical.

Absence of a known failing query is not evidence for an alternative
mechanism. A standard container or cleaner abstraction is not automatically
an acceptable substitute when it changes database-sized work or hot-path
ownership.

### 2. Pause new node-43 feature expansion

Outer aggregate ownership completes the ordered prerequisite closure, but it
does not complete node 43. Do not begin IN, FROM/coroutine, CTE, view,
constraint, trigger, window, or other new feature slices until this audit and
its blocking repairs are complete.

Node 43 remains active throughout the audit. Nodes 44 through 48 remain
blocked. This pause does not mark any node complete or reorder the dependency
graph.

The audit covers only implemented behavior. Unimplemented features are not
reported as divergences. When nodes 44 through 48 begin, their designs apply
this mechanism-baseline rule before implementation rather than relying on a
later corrective audit.

### 3. Maintain one complete implemented-mechanism inventory

`docs/sqlite-reference-audit.md` remains the canonical audit document. Its
next revision must inventory every implemented production mechanism reachable
through nodes 1 through 43.

The closed coverage population contains:

- every project-owned production translation unit;
- every project-owned header transitively used by a production target,
  including private implementation headers;
- every project-owned grammar, template, table, or other generated-source
  input that determines production behavior; and
- every public contract exposed by the resulting targets.

Third-party sources, tests, benchmarks, and development-only tools are outside
the mechanism population, but their evidence remains linked from the owning
entry. Every path in the closed population must appear in at least one
mechanism entry or in an explicit nonmechanism exclusion with a reason.
Grouping several files under one mechanism is allowed; silently omitting a
private helper or generated input is not.

Each inventory entry records:

- owning module-graph node and Modern source paths;
- pinned SQLite files, functions, and relevant line ranges;
- the native mechanism and its required prerequisites;
- the corresponding Modern mechanism and cross-layer seams;
- correctness, lifecycle, failure, ownership, and performance evidence;
- one disposition from the closed set below;
- uncertainty and untested boundaries; and
- any repair ADR, commit, pull request, and exact-tree evidence.

The allowed dispositions are:

- `reference-faithful`: the relevant native mechanism is preserved;
- `architectural-translation`: the same mechanism is split across the Modern
  DAG without changing its material invariants;
- `measured-alternative`: an accepted ADR and matched evidence authorize a
  different canonical mechanism;
- `repair-required`: a confirmed material divergence exists;
- `unresolved`: source mapping or evidence is incomplete; or
- `unimplemented`: the native mechanism belongs to an explicitly deferred
  feature.

No implemented production path or mechanism may be omitted or left without a
disposition or explicit nonmechanism exclusion. `repair-required` and
`unresolved` entries block feature expansion.

This is completeness of the implemented-mechanism inventory, not a claim that
every possible SQLite input has been exhaustively proven equivalent.

### 4. Use source mapping and executable evidence as the discovery process

For each audit cluster, the main development agent performs this sequence:

1. inventory the existing Modern abstractions, contracts, tests, and measured
   baselines;
2. read the complete relevant mechanism in the local pinned SQLite source,
   including callers, state owners, cleanup, and required helper algorithms;
3. write the source-to-source mechanism map before proposing a correction;
4. freeze a bounded behavior and failure matrix before changing production
   code;
5. establish or refresh matched timing and fixed-work baselines where the
   mechanism is performance sensitive;
6. classify the mechanism using section 3's dispositions; and
7. record confirmed differences with reproducible evidence.

Reviewers validate the completed mapping, classification, tests, and proposed
repair. They are not the primary semantic oracle and do not replace source
research, differential controls, or matched benchmarks.

Speculative concerns remain labeled `unresolved` until source or executable
evidence resolves them. A reviewer assertion alone does not become a defect.

### 5. Audit in dependency and risk order

The inventory proceeds sequentially through these clusters:

1. coding, database/record format, VFS, cache, journal, pager, B-tree, and
   transaction state;
2. SQL values, affinity, collation, functions, temporary storage, sorter,
   keyed relations, and aggregate state;
3. lexer, parser, catalog, binder, name/query ownership, and semantic copies;
4. logical planning, physical access, joins, subqueries, lowering, bytecode,
   VM execution, and read-session lifecycle; and
5. writable SQL, index maintenance, schema/statistics mutation, and public
   transaction outcomes.

Lower clusters are completed before auditing a dependent higher cluster.
Within a cluster, durability and corruption risks precede wrong results,
lifecycle differences, asymptotic performance risks, and constant-factor
allocation/copying risks.

Audit research may collect multiple findings, but production repair remains
strictly sequential. Do not implement separate findings in parallel.

### 6. Repair the root mechanism, not each observed symptom

Every `repair-required` entry receives one coherent repair design. The repair:

- begins with the SQLite mechanism and its prerequisite cluster;
- adds focused failing tests from the frozen matrix;
- changes the canonical shared mechanism rather than one consumer;
- covers normal, boundary, malformed, lifecycle, allocation, and injected
  failure behavior where applicable;
- preserves existing corpus records, benchmark inputs, thresholds, and
  deadlines;
- replays the affected inventory suffix when a lower mechanism changes; and
- passes exact-head review and the selected closing gates before merge.

Do not accumulate local conditionals around a custom mechanism when the
native mechanism already solves the complete problem. If a repair cannot be
made coherent without repeated exceptions, stop and replace the mechanism or
split at the last accepted boundary.

No compatibility shim, migration layer, duplicate executor, private sorter,
private relation, or second state machine is introduced to preserve a
superseded custom implementation.

After parity, any refactor or optimization away from the SQLite baseline is a
separate decision under ADR-0008 with a green baseline and matched evidence.

### 7. Require evidence appropriate to each mechanism

The audit reuses existing harnesses and adds only evidence required to resolve
an inventory entry:

- persistent formats: independent goldens, both interoperability directions,
  malformed inputs, and exact stored bytes or serial forms;
- durability: deterministic I/O faults, crash/recovery images, publication
  ordering, and cleanup retry state;
- SQL semantics: pinned differential rows, types, metadata, errors, failure
  phase, callback schedule, reset, and rebind behavior;
- planning and execution: plan/access identity where material, fixed VM/B-tree
  work, scaling across deterministic sizes, and matched result digests;
- hot paths: matched SQLite timing, allocations, bytes copied, comparisons,
  page/cache/VFS activity, and profiles where counters do not explain the gap;
  and
- ownership boundaries: exhaustive OOM/failure injection, pin/borrow cleanup,
  and same-object retry.

Timing alone never proves mechanism parity. Passing current unit tests alone
never closes an inventory entry.

### 8. Bound delivery and invalidation

The audit policy and complete inventory may share one documentation pull
request as two ordered commits. Production repairs do not share that pull
request.

Repairs use one coherent slice at a time and may batch only two through four
tightly related commits under ADR-0106. Persistent-state, SQL compiler,
runtime, and performance mechanisms are not combined merely to reduce CI
cost.

Changing a lower audited mechanism invalidates every dependent inventory row
whose evidence relied on the old mechanism. Reclassify and requalify that
suffix before feature expansion resumes.

The audit campaign is complete only when:

- every implemented production mechanism has a source mapping and disposition;
- every path in the closed production population is mapped or explicitly
  excluded as nonmechanism;
- no `repair-required` or `unresolved` entry remains;
- every `measured-alternative` has an accepted ADR and matched evidence;
- every repair is merged with exact-tree evidence;
- the final audit revision names the exact main commit and pinned SQLite
  identity;
- design and implementation review report no unresolved issue; and
- project graph and documentation consistency checks pass.

Only then may the next node-43 feature slice begin.

## Consequences

### Positive

- SQLite's proven algorithms and state machines become the default baseline
  across the implemented engine, not only persistent storage.
- Performance risks are found through source mapping and scaling evidence
  before they become feature-dependent patch chains.
- Intentional architectural differences remain explicit and reviewable.
- Every accepted alternative carries evidence rather than relying on aesthetic
  preference.
- Review returns to its intended role as final validation.

### Negative

- New feature delivery pauses while the implemented engine is inventoried and
  repaired.
- Complete native source mapping and matched baselines require substantial
  engineering time.
- Lower-layer repairs may invalidate higher-layer audit evidence.
- Some existing abstractions may be replaced even when current tests pass.

### Deferred

- Reproducing SQLite source-file boundaries, mutable AST/VDBE structures,
  opcode numbers, or internal ABI.
- Auditing WAL, full multi-connection locking, C API compatibility, and
  extensions before their owning nodes begin.
- Post-parity optimization without a demonstrated matched bottleneck.

## References

- ADR-0002: SQLite Compatibility Contract
- ADR-0003: Layered Dependency Architecture
- ADR-0005: Test-Driven Development
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0102: Change-Aware Verification
- ADR-0106: Batched Related Slice Delivery
- `docs/sqlite-reference-audit.md`
