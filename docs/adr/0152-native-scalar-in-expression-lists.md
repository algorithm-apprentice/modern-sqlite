# ADR-0152: Native Scalar IN Expression Lists

- Status: Accepted
- Date: 2026-10-25
- Module graph node: `implement-advanced-sql`
- Delivery: ADR-0053 slice 8, after the merged ADR-0151 audit closure

## Scope and completed reference mapping

Begin from PR #257's accepted main `eee2865`, tree
`672a99c599d8687ae133351efa6e42892ce12ddc`. The implemented-mechanism
inventory has no unresolved or repair-required entry. This is the next SQL
admission, not another audit or a declaration that node 43 is complete.

Admit scalar `IN (expression-list)` and `NOT IN (expression-list)`, including
empty lists, all current scalar consumers, aggregates, existing scalar-query
operands, aliases, correlated producers and coroutine boundaries.

Keep these distinct subsequent admissions explicitly unsupported:

- row-value operands and row-value lists;
- a SELECT/VALUES RHS, including a singleton scalar SELECT that native
  promotes into an IN query rather than applying scalar LIMIT 1;
- named-table/table-valued-function RHS shorthand; and
- new `IN_INDEX_LOOP`-driven table/index access paths.

Existing physical scan/index choices remain available; this slice implements
the scalar membership operation, not a new WHERE-loop family. It does not
substitute a linear list evaluator for native's large constant-set mechanism.

The pinned source map is:

| Mechanism | SQLite 3.54.0 source |
|---|---|
| Empty/singleton/list/query grammar transformations | `parse.y:1494-1565` |
| Pre-resolution constant walk, functions, parameters and boolean rewriting | `expr.c:2490-2678` |
| Construction heights, COLLATE and function attachments | `expr.c:175-189,1015-1074,1182-1276`; `parse.y:1458-1469`; `window.c:1311-1327` |
| Constructor-known false flags, AND deletion and literal null-test folding | `expr.c:994-1006,1150-1177`; `parse.y:1392-1453` |
| INTEGER constructor flags versus separated QNUMBER tokens | `parse.y:1201-1210,2147-2150`; `util.c:1310-1362` |
| Candidate RHS and native table/index choices, separate from list evaluation | `expr.c:3130-3160,3297-3518` |
| Constant-list choice: linear for nonconstant or at most two members | `expr.c:3198-3210,3470-3490` |
| Shared affinity derivation and SELECT-signature reuse | `expr.c:3530-3635` |
| Once-only list construction, key affinity/collation and insertion | `expr.c:3660-3890` |
| RHS-before-LHS order, linear comparisons and null accounting | `expr.c:4102-4240` |
| Borrowed-key search, empty/null results and first-key null check | `expr.c:4240-4408` |
| Ephemeral B-tree open, membership and cleanup | ADR-0117's `vdbe.c`/`btree.c` map; ADR-0031/0083/0143 lifecycle maps |

Forty-three original native public-API controls retain typed rows, error
phases, callback order/counts, VM work and EXPLAIN operations. They establish
the distinctions below before production changes. These are semantic and
fixed-work observations, not matched timing evidence.

### Observed non-interchangeable forms

- `NULL IN ()` is false and `NULL NOT IN ()` is true. An unknown identifier
  or a scalar SELECT on the LHS is discarded when native's local expression
  has no function flag. Parameters still contribute to statement metadata.
- A local function prevents that wholesale syntax deletion. It is resolved
  but not value-evaluated; aggregates still determine query grouping and run
  through ordinary aggregate phases. A function inside a child SELECT does
  not propagate that local flag across the query boundary.
- A singleton constant RHS becomes equality against unary plus. RHS explicit
  collation therefore matters in that form, whereas a nonconstant singleton
  and longer IN lists use only the LHS collation.
- A double-quoted unresolved name is not a parser-time constant even if later
  DQS binding produces text. `'a' IN ('A' COLLATE NOCASE)` is true, while its
  double-quoted-RHS counterpart is false.
- The native pre-resolution constant walk rewrites unquoted TRUE/FALSE before
  source lookup. Its earlier rewrites survive a later nonconstant child:
  with a column named TRUE containing zero and `x=1`,
  `1 IN (TRUE+x)` is false but `1 IN (x+TRUE)` is true.
- Linear membership short-circuits reached comparisons. Three constant terms
  instead build the complete ephemeral set before evaluating the LHS.
  `1 IN (1,abs(-9223372036854775808))` succeeds; adding a third constant
  term reports the integer-overflow step error.
- A nonconstant three-term list remains linear. Constant-set callbacks execute
  before LHS callbacks; nonconstant/small-list callbacks follow the LHS.
- A constant set inside a correlated scalar producer builds once per statement,
  not once per outer row. A WHERE alias copy and the projection own two
  materializations; direct ORDER BY result reuse owns one.
- `2 IN ((SELECT x ...))` examines the entire promoted RHS query. Unary plus
  around that SELECT, or a second list term, instead preserves scalar-value
  semantics. This slice must reject the promoted-query form rather than
  silently use the existing scalar destination.

## Canonical implementation inventory

Reuse the current pure parser and immutable postorder syntax arena,
`StatementBinder`, statement-global bound arenas, native alias/query-instance
rules, logical/physical expression consumers and `PlanLowerer`.

`BoundTruthHint`, discarded-value binding, pending aggregate finalization,
comparison affinity/collation, literal/parameter metadata and canonical
AND/OR/NOT lowering already provide the corresponding semantics. Extend their
existing traversals rather than adding an independent property registry.

`EphemeralRelation` already uses one canonical temporary B-tree in memory and
file modes, with lazy physical roots, shared statement budget, poison and
cleanup. Its encoded membership path is writing-only; its sealed prefix
lookup uses borrowed SQL-value keys. Existing closed private returns and
coroutine cancellation intentionally retire ordinary capabilities.

Those contracts cannot represent a reached-once immutable IN set retained
across private returns, correlated invocations or coroutine epochs. Encoding
every probe key, eagerly opening unreachable sets, rebuilding every outer row,
or weakening all closed-return checks is not an acceptable shortcut.

## Decision

### 1. Preserve syntax and native pre-resolution transformations separately

Add one source-preserving IN syntax payload with an operand, ordered expression
IDs, negation and operator/list spans. Keep postorder/reference/span/depth
validation and exact tail behavior. Parser syntax remains independent of the
catalog and function registry.
Retain the native local-function-presence fact on IN syntax boundaries through
one shared pure helper and validate it with the syntax arena. This keeps
source-preserving nested list normalization linear without entering SELECTs
or allocating another expression-property container.

Represent native semantic normalization in one lazily allocated binder
annotation over the existing syntax IDs, allocated only for statements that
contain IN. It records the required parser-equivalent form and boolean
rewrites; it is not another AST, expression arena or evaluator.

Perform the singleton pre-resolution constant walk in native child order,
using the canonical function selector and preserving partial boolean rewrites
even if a later child makes the expression nonconstant. Do not replace it
with post-binding determinism, DQS text conversion or a final error-code test.
Use the existing native-equivalent source/constant helpers for the separate
post-binding list classification.

Empty-list normalization skips LHS semantic binding only when the local
native function flag permits deletion. Otherwise bind it in the existing
discarded-value mode: names, functions, arity and aggregate ownership still
apply, but unreachable scalar widths/collations and value evaluation do not
become new requirements. Preserve aggregate registrations and global parameter
assignment. Boolean-keyword identity follows the native normalized form,
not merely the constant integer result.

Update effective resolution height through that same annotation. Retaining
immutable source syntax must not charge deleted native semantic subtrees or
omit the singleton unary-plus/NOT height. Parser construction limits remain.
Validate every normalized intermediate construction in postorder before a
later empty list can delete it. Cache those heights and native unary-plus
provenance in the same annotation, using the canonical height calculation for
both this check and subsequent resolution. This performs no name binding of
discarded operands and adds no ordinary-statement allocation.
Match native constructor ownership: COLLATE starts at height one, and attached
FILTER/local ORDER BY expressions do not raise their function owner's height.
Their previously constructed operand/attachment expressions still receive
their own earlier height checks.
Keep constructor-known integer/boolean constants and function presence in that
same annotation. Native COLLATE does not propagate its operand's function or
known-false flags. Reuse one pure function-presence kernel for recursive syntax
queries and postorder annotation. Preserve native false-AND deletion and
literal null-test folding without binding their deleted operands; distinguish
the resulting integer constants from TRUE/FALSE keyword identity.
Check the native function-constructor argument bound before deletion as well;
this does not reactivate discarded semantic function-name or arity lookup.
Classify INTEGER constructor truth flags through one borrowed 32-bit token
helper, shared with ordinary literal truth hints. Record its zero flag before
AND construction and IN constant classification. Separated QNUMBER tokens,
REAL, unary signs and COLLATE do not acquire that flag. Do not materialize or
validate a discarded large literal merely to classify constructor flags.
Preserve native `SQLITE_ERROR` for expression-construction depth and function
argument-count failures through the existing shared parse/bind error mapping.
Do not change their limits, the structural/recursive parser guards, or other
resource-error categories.

### 2. Keep the native linear and reusable-set choices

Use existing comparison/branch primitives for nonconstant lists and lists of
at most two terms. Evaluate the LHS once, then reached RHS values left to
right; preserve match short-circuiting, NULL accumulation and NOT semantics.

For longer constant lists, emit a lazy reusable membership capability.
Build every RHS key before evaluating the LHS, once per execution. Use the
canonical record encoder, LHS-derived key affinity, native REAL-to-NUMERIC
key normalization and LHS-only collation. Do not invent a hash table, vector
set, comparator, SQL executor or statement cache.

Allocate one set descriptor per lowered semantic occurrence, not globally by
bound-expression ID. This preserves alias-copy materializations while direct
result-register reuse remains reuse. Existing nested scalar values inside a
multi-term list keep their current query owners and destinations.

### 3. Add one concrete reusable membership purpose to the current relation

Extend the current relation purpose with `ReusableMembership`, preserving all
existing purposes and default behavior. Its first admission is one ascending,
NULL-first key field with no payload suffix; duplicates use current membership
semantics. It remains the same `EphemeralRelation`, relation arena, Pager,
codec, comparison owner and physical B-tree.

A lazy readiness instruction creates the logical handle only when reached.
Its build edge owns a writing set; its ready edge owns a sealed set. Extend
the current sealing primitive to publish immutable membership readiness.
When NULL distinction is required, cache null presence once by positioning
the canonical index cursor at the first key and comparing its type/key with
NULL, without copying a whole payload. Omit this work when native's false and
NULL destinations coincide.

Probe borrowed register values through the current index seek/comparison
primitives, not a newly encoded ByteBuffer per probe. Share the seek kernel
with existing sealed lookup rather than duplicating its algorithm. Retain
the reusable cursor and comparison owner across probes, invalidating record
views before position changes and closing them through existing cleanup.

One typed membership branch expresses true, false and unknown destinations.
Value consumers materialize 1, 0 or NULL through current register primitives;
predicate consumers retain native coincident-destination handling. Validate
that a program requesting NULL distinction has the corresponding sealed
null-status contract. NOT swaps true/false destinations, never NULL.

### 4. Verify statement lifetime without cross-region capability sharing

Reusable membership has one lexical owner, like every current mutable
descriptor, but its sealed contents live until whole-execution cleanup.
No caller/callee ID borrowing or second runtime handle container is added.

Extend the existing relation-state lattice narrowly with closed-or-sealed
readiness for this purpose. The lazy readiness instruction refines it into
writing or sealed edges. Writes after sealing and probing before publication
remain invalid. A private normal return, coroutine yield/end or cancellation
may preserve only an absent or sealed reusable set, never a partial build.
All other private capabilities retain their closed-return rules.

Private-region entry accounts for first and repeated invocations. Preserve
the same lexical ownership/effect checks, existing summaries and analysis
budgets; do not add a parallel resource visitor. Coroutine-scope cancellation
preserves sealed statement-lived membership, while whole VM error, halt,
reset/rebind, automatic reprepare, finalization and destruction close every
set and release its roots/pins. Programs with no reusable membership retain
their existing allocation path.

## Test-first ordered delivery

Use one related ADR-0106 batch, with independently reviewed buildable commits:

1. Add the required sealed/reusable relation and verified VM lifecycle
   capability through focused owning red/green tests and complete fast units.
   Admit no SQL grammar in this capability commit.
2. Add the complete scalar-list vertical slice from public-session red through
   parser, binder, plans, lowering and VM. Do not commit a parser-only prefix.
3. Publish the portable qualification and native/matched-performance evidence
   on the exact batch head. Each preceding behavior-owning commit already
   includes its differential, malformed, boundary, lifecycle and failure tests;
   this is not a deferred testing phase.

Reuse current VFS, temporary-storage, cursor, allocation, model, trace/oracle
and grammar-fuzz helpers. Preserve frozen old cases and all original limits,
thresholds and deadlines. Include unreachable/empty sets, late scalar errors,
NULL and mixed affinities, singleton DQS/boolean normalization, first-RHS
collation, alias cache separation, statement reuse and both temporary modes.

Stateful capability tests own malformed programs: unknown purpose/IDs,
unsealed return/yield, wrong-region references, mutation after publication,
conditional readiness joins, repeated private calls/coroutine epochs,
cancellation and whole-execution cleanup. No failure path silently publishes
an empty or successful-looking set.

Record native fixed work before changing the mechanism; isolate ordinary
timings from diagnostics and builds. Verify large constant-list membership
scales through the B-tree and nonconstant lists retain their native linear
evaluation. Mark SELECT RHS, named shorthand, vector forms and new IN-driven
WHERE loops as the explicit subsequent scope, not falsely implemented code.
Final integrated GPT-6.1 Sol review and exact-head local/hosted acceptance
precede merge and the next coherent slice.

## Matched qualification contract

Reuse the existing ordinary and diagnostic ORDER BY benchmark binaries'
canonical typed-row replay, configuration, result digest and phase separation.
Their established row-consumer workload class also serves aggregate and
compound SQL; this does not add an IN executor or require a sorter.
Append declarative IN cases without changing any existing case, fixture,
workload, threshold or deadline.

Use the pinned 65,536-row `order_by_performance/ordered.db`, 4 KiB pages,
512 cache pages, DELETE/FULL, mmap zero, and the existing single-threaded
profile. Every query explicitly orders by the INTEGER PRIMARY KEY, so both
engines use rowid order and report zero sort operations.

The bounded baseline has five cases:

- `in-linear-two-memory`: `id IN (1,2)`;
- `in-constant-256-memory`: `id IN (1,...,256)`;
- `in-constant-256-file`: the same constant set in file temporary mode;
- `in-dynamic-three-memory`: `id IN (1,id+1,NULL)`; and
- `in-correlated-constant-memory`: a scalar SELECT capturing the outer `id`
  and probing the same constant set.

Each projection reads every source row. Use one warmup, eight measured query
executions per repetition, one separate diagnostic execution, and the
unchanged three-round/three-repetition paired schedule: nine timing samples
per engine and case. Retain the 5 ms minimum and 10/1 CPU/wall guards before
collecting measurements. Memory/file temporary budgets are the existing
64 MiB/2 MiB profiles.

A thin family contract adapter pins those SQL/configuration/work/digest
records and captures its own source identity through the shared runner.
Reuse the shared exact-type contract validator with the caller's predeclared
guard; the existing DISTINCT/aggregate guard remains unchanged.
Validate actual SQLite full-scan count 65,535, zero sorts and zero spill;
retain all ordinary diagnostics and allocation counters without timing
instrumentation in the production library. Compute independent typed
FNV-1a vectors from rowids and verify them in both engines.
Keep the inherited row-replay protocol tag instead of adding a parallel
measurement pipeline. Publish the clean measured source prefix and raw
paired reports, then publish the portable archive and its validation
registration as an evidence suffix. That suffix changes no engine,
workload, runner, benchmark target or benchmark build flags.

Keep row-replay workload construction in one canonical owner. Reuse the
existing `ReadWorkload` factory for ordering, scalar IN, compounds and
aggregates instead of copying its fixture/profile initialization into each
family or case. This is a green-baseline declaration refactor: preserve every
SQL, iteration count, work fingerprint, temporary budget and diagnostic
contract. Keep independent native outcomes and raw measured evidence intact;
do not add a new schema, runtime configuration layer or test framework merely
to reduce line counts.
