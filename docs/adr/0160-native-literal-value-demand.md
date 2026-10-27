# ADR-0160: Native Literal Materialization and Value Demand

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: bounded literal prerequisite before IN query-RHS admission

## Context and completed source mapping

Start from PR #264's verified squash `b0bc196`, reviewed/qualified/merged tree
`ec82f0f45906e266acf6e70166dde424c2654412`. Actual resulting main CI and
full quality passed. Node43 remains active.

Research for native IN stored-access versus materialization confirmed an
existing implemented-surface difference: native
`SELECT EXISTS(SELECT 0x10000000000000000)` returns1, while Modern reports
a literal preparation error. An ignored EXISTS ORDER BY, nested scalar
output, singleton VALUES, DISTINCT, UNION ALL, single-row-demand UNION and
unused aggregate output have the same value-demand distinction.

Executed required WHERE/group/HAVING, set-comparison, scalar LIMIT0 and
multirow VALUES-constructor controls retain their errors. Native mutation
controls also accept overwritten bad literals and ignored EXISTS outputs,
but reject the effective assignment even with WHERE0. Missing names still
fail. These are preparation/value-demand observations, not timing evidence.

The pinned SQLite3.54.0 source map is:

| Responsibility | Native source |
| --- | --- |
| Integer/REAL literal generation and range handling | `expr.c:4408-4460,5208-5221` |
| TEXT/BLOB value creation | `expr.c:5223-5260` |
| Unary literal fast paths versus value operators | `expr.c:5372-5395,5656-5660` |
| EXISTS projection elision and output completion | `select.c:1256-1269,1467-1471,3270-3275` |
| Scalar/EXISTS subroutine and LIMIT normalization | `expr.c:3914-4050` |
| Discardable destination ordering/distinctness | `select.c:7756-7778` |
| Bounded native integer identity | `expr.c:2959-3007` |
| QNUMBER normalization | `parse.y:2147-2150`; `sqlite3DequoteNumber` |
| VALUES constructor-owned value requirements | ADR-0159's `insert.c`/`expr.c` map |
| Overwritten UPDATE ownership and value generation | ADR-0157's `update.c` map |

Native retains a lexically constructed literal until value generation
actually consumes it. Modern `BindLiteral` and the unary literal guard
materialize values even in the current non-value binding mode.
`ClassifyOrderByInteger` additionally materializes an entire SQL value just
to decide whether the token is a bounded ordinal.

Inventory the existing immutable syntax/source owner, `BindScope`,
`binding_only_`, `BoundQueryRequiresResultValues`, canonical result-reference
validation/copies and shared statement expression arena. Reuse
`DequoteSqlToken`, SQL numeric conversion, `ByteBuffer`, expression properties,
`StatementLoweringState::literal_constants` and the existing constant emitter.
The VM must still receive only owned SQL values and verified bytecode.

## Decision

### 1. Separate a bound literal token from a materialized SQL value

Add one typed unmaterialized-literal alternative in the existing bound
expression variant. It contains the admitted literal kind and a borrowed
UTF-8 token view, not a SQL value, AST node, callback or separate registry.

Published views borrow only the immutable SQL copy in `BoundSelect::Impl`,
never the soon-to-be-destroyed syntax tree or caller input. That implementation
owner is retained unchanged across bound/query moves and template copies.
Internally transient materialization descriptors may borrow for the duration
of their call. No token view enters the bytecode program or VM.

When value binding is not required, retain the token rather than parsing
its integer/REAL, dequoting an ordinary TEXT value or allocating BLOB bytes.
Do not fabricate NULL, suppress an error from a required consumer or
successfully bind an expression twice.

Keep NULL and boolean-keyword identity through the current representation.
TRUE/FALSE source-name lookup and DQS identifier lookup happen before the
unmaterialized-literal decision. Current-time literals remain explicitly
unsupported; this does not admit a new function or syntax surface.

### 2. One canonical leaf materializer and one metadata classifier

Extract the existing positive literal conversion kernel and reuse it for
ordinary binding, constant-default leaf materialization and deferred constant
emission. Preserve current numeric, UTF-8/TEXT, BLOB, allocation and typed
error contracts. Expected materialization failures remain typed value
requirements; structural token/span errors remain structural errors.

Reuse a single bounded nonnegative32-bit integer classifier for ordinal and
integer-one metadata rather than materializing a64-bit SQL value. Preserve
the constructor's distinct zero/boolean flags. Executed QNUMBER controls
show that `1_0`/`0_1` and their ordinary unary forms are resolved ordinals;
they must not be changed based on the disproved nonordinal hypothesis.
Large hexadecimal tokens are nonordinals, not classification errors.

Retain the current unary transparency/barriers and valid signed-minimum
special case. The unary guard may not report a value-only hexadecimal error
when its value is not needed. Required unary interpretation is otherwise
unchanged in this bounded slice.

### 3. Materialize only at an actual value consumer

Keep ordinary successful required literal binding on its current path.
For unmaterialized literals that later reach a real value emitter, call the
same materializer and publish its owned result through the existing constant
pool and per-expression optional constant ID. Repeated emission reuses that
canonical constant; add no second value cache or execution state.
The existing typed lowering error retains a literal materializer's `BindError`
when generation reaches a deferred value. Preserve its base code, source span
and detail instead of relabeling a native value error as an internal failure.
Use one tagged lowering cause for the existing program error or an immutable,
owned literal error. Allocate the latter only on that cold failure path;
successful lowering must not carry a second inline diagnostic. Native MSVC
measurement found that two inline optional errors doubled the lowering error
from72 to144 bytes and broke the unchanged1000-level expression control in
lowering. The tagged owned-cause design must retain the original carrier
footprint and pass that exact control without changing stack or depth limits.

Extend the existing expression/dependency/equivalence visitors and bounded
metadata helpers explicitly. Unmaterialized literals have no source/query
dependency and retain native constant/ordinal facts without pretending to
contain a `SqlValue`. Preserve required alias/group/set validation and
name-binding-only captures.

The current query, aggregate, compound, ordering and mutation demand decisions
remain authoritative. Constructor-owned VALUES rows still perform their
required early preparation, including both first and incoming coroutine
rows. An ignored probability in native incoming likelihood remains metadata
only under ADR-0159.

No separate read/write parser, literal representation in the VM, executor,
source copy, owning byte type, comparison algorithm or query owner is added.
Do not increase the ordinary bound-expression footprint merely to retain
optional invalid-input diagnostics; measure the variant footprint.

## Bounded scope and remaining gate

This corrects existing ignored-value literal preparation through shared
read/write mechanisms. It does not admit IN query RHS or fix every
required-value diagnostic-order issue.

Executed controls separately confirm a required unary boundary divergence:
native rejects direct negation of hexadecimal INT64_MIN, while Modern
currently returns REAL. Collate/likelihood barriers behave differently and
must remain distinct. That confirmed mechanism requires its own ordered,
test-first correction before new IN feature admission; do not silently
change it as part of this value-demand refactor.

## Test-first, ownership and delivery

Record a focused public runtime red for ignored EXISTS hexadecimal output.
Add read/write paired required/ignored cases, constructor controls,
parameter/column metadata, nested query/alias/group/set consumers and typed
name failures. Preserve required WHERE0/LIMIT0 errors.

Independently attest literal types/bits with pinned native vectors. Test
source ownership after caller/tree retirement and bound-owner moves, lazy
constant publication, malformed descriptors, reset/rebind, statement failure
and complete owning allocation-failure sweeps without changing ceilings.
Required current behavior and unsupported boundaries remain explicit.

Review the design before implementation. Each coherent commit retains
focused red/green or a genuine green refactor baseline, complete fast units,
source review and ledger evidence. Run the actual change-aware plan and
necessary native/compiler/sanitizer/fuzz selections against the final exact
head. ADR-0053 still defers new timing families and optimization.

## Recorded implementation evidence

The public ignored-EXISTS hexadecimal test failed before production changes
and now passes. A source-owner regression additionally caught the parser's
identifier-shaped DQS fallback; both literal-shaped and identifier-shaped
fallbacks now reuse the same owner-backed token publication after lookup.
Pinned execution confirmed that the membership fixture's real `t.x` wins
over a HAVING alias; the paired required-alias control uses `literal_value`.

The focused22-case selection includes the unchanged maximum construction
depth and function-attachment controls. All2052 native isolated unit cases
passed, including the complete binder/lowering allocation-failure sweeps.
The binder adds four complete failure scenarios under its unchanged1024
ceiling; lowering adds four deferred/ignored value scenarios under its
unchanged existing allocation limits. Long TEXT/BLOB/numeric paired
allocation controls prove that ignored values avoid owned-value allocation.

MSVC measurements retain `BoundExpression=112`, payload80, token24 and
lowering error72 bytes. The arithmetic emitter's measured frame reservation
fell from the rejected inline-error design's `0x388` to `0x298`, and the
original1000-level runtime control passed with the default stack.

The existing subquery shard retains all248 historical cases and adds36
literal-demand cases, for284 total below the unchanged512 cap. Regeneration
used the actual pinned GNU reference library; all historical outcomes and
canonical compiler provenance remained unchanged. The native
282-comparison/two-unsupported-boundary shard passes against the current
Modern trace. These results do not replace final-head compiler, sanitizer,
review or hosted acceptance and do not close the separate unary finding.
