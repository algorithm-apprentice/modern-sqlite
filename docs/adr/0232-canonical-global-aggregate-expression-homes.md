# ADR-0232: Canonical Global Aggregate Expression Homes

- Status: Accepted
- Date: 2026-11-08
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #307 main `0f653bfb`, ledger290
- Related contracts: ADR-0036/0067/0082/0106/0111/0216/0218/0226/0231

## Confirmed failure and bounded inventory

The remaining native caller inventory exposes an existing violation of
ADR-0218's canonical register-binding rule in extracted global aggregate access.
For a nullable indexed column:

```sql
CREATE TABLE t(v INTEGER);
CREATE INDEX ix ON t(v);
INSERT INTO t VALUES(NULL),(5),(2);
SELECT (SELECT min(v) FROM t WHERE v IS ?1);
```

With the parameter unbound, pinned SQLite prepares successfully and returns
NULL. Both current ReadSession and WriteSession fail preparation because
the lowered bytecode fails verification. The direct lowerer diagnostic reports
`kUninitializedRegister` at the private call, instruction1/detail3 in this fixture.
The facade messages have different prefixes but the same lowering failure.

Eight paired controls cover MIN/MAX, a parameter or literal NULL key, and
top-level or private-query placement. All four top-level controls match native;
all four private forms fail in both facades. The retained source283 WriteSession
image reproduces every current protocol, so this is not introduced by ADR-0231.
That is an authenticated historical-image control, not a full historical rebuild.
These probes are research evidence, not the focused test-first red.

| Responsibility | Canonical source |
| --- | --- |
| Select the global aggregate strategy for an ordinary nested aggregate | `PlanLowerer::PrepareReadLayout`, `EmitReadPlan`, `EmitGlobalAggregatePlan` |
| Enter the real typed private owner | `EmitReadSubroutine`, `BeginPrivateExpressionScope`; ADR-0218 |
| Allocate or retrieve the actual home | `PlanLowerer::Home`, `ExpressionRegister` |
| Emit an index equality key at that home | `EmitIndexKeyExpression`, `EmitIndexKeyExpressions` |
| Reconstruct the wrong register for the nullable IS branch | `global_aggregate_access.cpp:86-88,1713-1728` |
| Other uses of the same duplicate conversion | Global aggregate rowid range start/end, `global_aggregate_access.cpp:402-434` |
| Reject an uninitialized callee input rather than invent a value | Canonical subroutine summaries and definite-state verification; ADR-0082 |
| Preserve actual returned equality registers and owned key cells | Pinned `sqlite3.c:165330-165378,165488-165523` |
| Separate callable temporary ownership | Pinned `sqlite3.c:120501-120560`; ADR-0218 |

`GlobalAggregateAccess` currently has a second `Home` implementation that
converts `BoundExpressionId` directly into `RegisterId`. That happens to match
the main-body prefix. It does not match the owner-aware private binding used
when the shared emitter actually defines an index key.

Consequently, the runtime IS branch reads an unwritten main-prefix register
instead of the private key that was just emitted. The verifier correctly
attributes that read to the subroutine's required inputs and rejects its call.
The two rowid range sites also bypass the same canonical binding boundary,
even when their explicit writes make a particular program pass verification.

## Decision

### Delegate every extracted home request to the existing owner

Expose the existing typed `Home(BoundExpressionId)` operation through the
private `GlobalAggregateAccessServices` interface. `PlanLowerer::Home` remains
the sole implementation and overrides that service contract.

Remove the extracted module's independent bound-ID conversion. Its rowid
start/end destinations and runtime IS guard request the canonical home and
propagate a failed `LoweringResult` before emitting the dependent operation.
Do not add another register table, allocator, expression cache or conversion
algorithm.

Keep the current main prefix and private subroutine/coroutine binding rules.
A home requested in a private body is allocated lazily through the existing
checked builder and reused in that same owner. Correlated inline work retains
its caller's actual instruction owner. No lexical-context switch may silently
change that ownership.

This service exposes an existing allocation/access boundary, not a new
expression-generation mode. Preserve fixed-output emission, the actual child
results established by ADR-0231, and the separate caller intent of rowid
lookup, range bounds and scalar argument blocks.

### Preserve SQL, verification and resource boundaries

Do not remove the NULL guard, initialize unrelated registers to hide the read,
move affinity application, regenerate a key, or relax verifier summaries.
Preserve selected index access, MIN/MAX NULL-region passes, seek direction,
residual filters, aggregate state, query caching, result copies and cleanup.

Keep the existing typed allocation/range errors and compiler/session OOM
boundary. Main-only programs retain their bytecode/register/allocation path.
Private rowid ranges may acquire the reached private homes required by
ADR-0218; measure that bounded correction rather than reserving maximum space
or raising a quota.

No public bytecode layout, VM instruction, persistent format, callback
schedule, constant factoring, deferred queue or SQL feature is added.

## Focused proof and delivery

After independent design acceptance:

1. Add an owning lowerer regression for the verified private indexed MIN/MAX
   IS-key case. Observe its existing `kUninitializedRegister` failure before
   changing production code.
2. Delegate all three extracted home uses to the canonical service.
3. Cover MIN/MAX, nullable parameter and literal keys, non-NULL and NULL
   reset/rebinding, and private rowid range start/end ownership. Use existing
   catalogs, fixtures, result helpers and native facades.
4. Add the relevant private allocation case to the existing lowerer OOM owner
   and retain checked register-limit evidence without increasing ceilings.
5. Run the focused regression, complete fast-unit tier and actual change-aware
   closing plan. Preserve current native/read/write protocol observations,
   callback controls and meaningful unchanged main-only allocation evidence.

The existing paired eight-case fixture supplies independent native comparison
through both session facades. The direct diagnostic retains the exact lowerer
cause; neither substitute alone for the owning unit regression.

Review the implementation and source-matched evidence before a normal
node/ADR-traced commit and append-only ledger entry. Exact final local, original
hosted, integrated and actual-main gates remain mandatory.

This closes a missed consumer of the existing private-register contract.
The broader original-generation-site, caller factoring mode, retained result,
shared eager constructor and preparation-error work remains separate.
It does not admit IN or complete node43.
