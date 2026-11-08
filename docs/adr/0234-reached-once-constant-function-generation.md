# ADR-0234: Reached-Once Constant Function Generation

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #309 main `509bdf09`, ledger299
- Related contracts: ADR-0106/0111/0218/0219/0221/0226/0231/0233

## Bounded requirement and completed mapping

The current lowerer invokes a constant deterministic scalar once per input
row. The retained eight-case callback population has five differences:
native/Modern1/3 for projection, conditional and correlated-query results;
2/4 for an alias guard and projection; and2/6 for two source occurrences.
Empty, LIMIT0 and the existing uncorrelated-query cache controls agree.

The native function-root path independently requests RunJustOnce, even when
its surrounding result list does not request general constant factoring.
Its function-bearing branch emits an inline once gate, generates the body
immediately with factoring disabled, and returns a retained register.
This branch can be translated without adding a deferred queue or changing
the current preparation-error owner.

| Responsibility | Canonical source |
| --- | --- |
| Constant function-root admission before inline-function dispatch | `sqlite3.c:118257-118335` |
| Reached gate, disabled body and retained destination | `sqlite3.c:118698-118740` |
| ConstantNotJoin and function metadata | `sqlite3.c:115297-115391,115496-115509` |
| IN RHS-first generation and disabled LHS | `sqlite3.c:116921-116995` |
| Ordinary query caller mode versus original VALUES construction | `sqlite3.c:116711-116855,139930-140003,151979-151986` |
| Canonical constant traversal | `binder_detail::InspectConstantExpression` |
| Effective outer-ON ownership | `PlanLowerer::NativeTruthHint`, `BoundOnOrigin` |
| Iterative value/control completion | ADR-0231, `EmitOperationLoop` |
| Physical register ownership and allocation | ADR-0218, `ExpressionRegister`, `ProgramBuilder::AllocateRegisters` |
| Invocation epoch and terminal cleanup | ADR-0219, existing VM once execution |
| Protected entry construction | ADR-0221, existing query-result initialization |

Additional main299 observations cover fourteen grouped, alias and membership
queries with equal rows and no preparation callbacks. Twelve bounded
existing-bytecode controls cover shared read-once membership in both temporary
modes, moved owned bytes, reset, scalar failure and interruption recovery.
These establish available mechanisms, not permission to share arbitrary
registers or to claim a complete native generator.

## Decision

### Implement the inline reached branch at actual generation sites

Before ordinary function payload generation, and after the existing grouped
representative selection, admit an eligible function root only when:

- the actual caller's constant-factoring mode is enabled;
- the existing constant inspector accepts the tree under ConstantNotJoin's
  effective outer-ON exclusion; and
- the retained native construction property `has_function_flag` is set.

Function roots include the current ordinary scalar and inline special-function
representations, not just `BoundScalarCallExpression`. Preserve wrapper
dispatch and the distinction between construction flags and semantic function
containment. Do not invent a slow-changing-function category.

Extract the existing effective-ON calculation used by `NativeTruthHint` and
reuse it for the constant inspector's observer. Invalid core/origin metadata
remains a typed invariant error. Columns, queries and aggregates retain the
canonical inspector's rejection; determinism alone does not make their
arguments constant.

For each admitted actual emission, allocate one fresh retained result and one
existing once site through `ProgramBuilder`. Emit the once gate inline at
that location. Its already-seen edge skips the body, not the consumer's result
copy. Generate the same root immediately through the same continuation with
factoring disabled, bind the existing label mechanism, then copy its concrete
retained result into the caller's requested destination.

The result is distinct from expression scratch, scalar argument blocks,
coroutine output/receiver ranges and mutable key-affinity cells. Do not memoize
it by expression ID, value equality, function name or SQL text. Separate
ordinary occurrences and alias-use generation sites remain separate.

No new callable, runtime container, opcode, evaluator, resource-sharing proof
or verifier relaxation is introduced. Inline emission preserves the current
logical and physical placement of preparation operations and failures.
Nested functions in an admitted body execute under the disabled mode, so
they do not recursively acquire redundant gates.

### Carry the caller mode through existing continuations and definitions

Use one statement-wide current mode and carry its value in the existing
expression request. Scope installation/restoration around each continuation
step; child requests inherit it unless an actual native boundary overrides
it. Do not leave a disabled mode installed across unrelated siblings.

For both membership implementations, generate the selected RHS under the
incoming mode and disable factoring only for the LHS value request. Restore
the incoming mode for linear RHS comparisons and enclosing expressions.

Captured queries emitted inline inherit the current mode. An uncorrelated
query registered for delayed emission captures the mode of its first real
generation request in its existing call binding; its later definition restores
that mode before registering child descriptors or emitting its body.

Captured queries can also retain delayed native coroutine definitions. Save
the current mode when each existing `NativeProducer` is registered, alongside
its entry/destination metadata, and restore it in `EmitNativeCoroutineBodies`
before emitting that definition. This applies to captured ordered compounds
as well as main and uncorrelated queries. Producer zero emitted inline keeps
the current caller mode. A correlated ordered compound under IN must not
silently regain enabled factoring when its remaining SELECT arms are emitted
after the inline caller has returned.

Keep the modes with these actual existing definition owners, not in a second
query-mode registry. Do not derive them from final placement, a restored
lexical parent's ambient mode, or merely entering a prepared query frame.

An original eager VALUES row uses its construction-time enabled mode, including
when its eventual consumer is under a disabled IN LHS. Consumer-filtered
fallback rows inherit their actual caller. Share one checked row-schedule
traversal between streaming and ordered VALUES emission; retain original
chunk coverage, ordering and failure checks.

This mode transport does not introduce general temporary/factorable request
handling. Existing fixed-destination services and ADR-0231 completion checks
remain in force.

### Reuse the protected entry initializer

The verifier must still prove every already-seen read initialized. Extend the
existing out-of-line entry-constant construction, rather than weakening that
proof or initializing a retained result again on each call.

Record only actual new once-result register IDs in compiler-local state and
initialize them to NULL through the same protected entry gateway as existing
query results. The once body is their only later writer. No callback, parameter
evaluation, membership construction or other SQL operation moves to entry.

Make the gateway available when the bound arena contains a potentially
function-bearing root, as well as when query results already require it.
This cheap metadata check is not another value-generation traversal. It may
retain an empty gateway for a function whose contextual admission is later
rejected; record that small non-target instruction cost explicitly. Statements
with neither role keep their existing entry path.

Rename the existing initialization helpers to reflect their shared role;
do not add a competing initializer, result-value vector or per-expression
runtime cache. Allocate only actual results and preserve all builder, VM,
analysis, allocation, instruction and deadline limits.

### Preserve explicit remaining boundaries

This is the function-bearing, reached branch at current physical generation
sites. It does not activate the non-function deferred branch or reusable
deferred-expression search, change literal/width/collation arbitration, or
move binder errors into the lowerer.

It also does not merge separately emitted consumers of an original VALUES
producer. ADR-0233's original-site identities remain the required input to
that subsequent ownership correction. A physical once gate in each such
consumer is not claimed to be one native original-generation site.

Grouped output has another existing logical-site boundary: native generates
one output subroutine and calls it at a group change and at the final group
(`sqlite3.c:158251,158281,158299-158310`). The current lowerer emits that output
twice. The first implementation's physical-site gates therefore leave
native/Modern1/2 calls for a constant result grouped by another key, and3/5
for the nested-function result control. Both retain equal rows. These are
preserved unresolved observations, not a successful fourteen-case parity
claim. A separately reviewed grouped-output identity correction belongs to
the same related delivery batch before this runtime change is merged.

Preserve and report these remaining differences rather than silently treating
ordinary-site success as general parity. No global cache, signed-token-only
patch, speculative rebinding, fake query, shared mutable coroutine state or
new IN admission is permitted.

This narrowly authorizes the inline runtime branch while the broader
generation/diagnostic design remains open. Its isolation depends on immediate
body generation and unchanged preparation outcomes, not on declaring those
outcomes native-compatible.

## Test-first implementation and delivery

After independent design acceptance, add an owning lowerer regression using
the existing deterministic callback that returns its argument. Record the
current three-versus-one failure before production changes.

Cover distinct occurrences, alias guard/result sites, empty and LIMIT0
reachability, conditional arms, grouped representative selection, correlated
and delayed query contexts, IN LHS suppression and RHS inheritance, retained
outer-ON versus reduced joins, and original eager versus fallback VALUES
mode. Include a correlated ordered compound under IN whose SELECT-arm
coroutines are emitted later, paired with its enabled non-IN control.
Keep nondeterministic and column-dependent controls.

Exercise reset/rebind, owned TEXT/BLOB results and typed callback failure
through generated programs. Reuse existing VM epoch and ownership controls,
and keep the complete owning lowerer allocation-failure sweep without raising
its ceilings. Extend that sweep only with genuinely newly exercised paths.

Record a matched lowering-allocation and fixed-work baseline before production
changes. Measure actual sites, registers, copies, emitted/executed instructions
and allocation/retirement deltas; no timing improvement is presumed.
Rebuild observers against their consumed definitions. Retain the115 native/
write and103 read preparation protocols unchanged, and distinguish newly
closed ordinary callback cases from still-open original-producer and deferred
cases.

Each source requires focused red/green, complete fast units, current compiler
and checker owners, independent implementation review and node/ADR/ledger
traceability. Run the actual change-aware final local/hosted/integrated/main
gates before squash delivery. Node43 and later DAG nodes remain incomplete.
