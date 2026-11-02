# ADR-0191: Native Function Construction Properties

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted ADR-0190 source `3a074e5c`, tree `b4e62570`
- Related contracts: ADR-0159, ADR-0162/0164 and ADR-0106/0111

## Completed source mapping

The shared logical selector is accepted with its complete local gates and
actual prefix CI41380150616. Its draft #283 is not final integrated delivery.
Further direct constructor mapping confirms that value preparation cannot
be replaced by an eager recursive materializer.

Native `select.c:2539-2546` enables constant factoring when the first VDBE is
created. `expr.c:5895-5947` distinguishes expressions carrying `EP_HasFunc`:
function-bearing constant expressions use reached-once code; function-free
ones are queued for final initialization. `build.c:145-155,241-251` generates
that deferred work only after earlier preparation succeeded.
Constructor-generated code and later value materialization therefore have
distinct owners even within one VALUES row.

The current 30-control constructor matrix also distinguishes first-row source
type metadata from incoming value code. `select.c:2404-2490,6497-6570` resolves
ephemeral-source column collations after names. An incoming logically pruned
comparison can omit a missing collation, while the retained first row may
still need that collation as source metadata. These observations prevent a
blanket early-value deferral or a guessed collation-error ordering.

This source publishes the missing native function-construction fact needed
by the subsequent owner/ordering correction. It does not yet change value
preparation, factoring, runtime calls or error selection.

The pinned flag mechanism is:

| Responsibility | Native source |
|---|---|
| Propagated construction flag | `sqliteInt.h:3144,3174-3177` |
| Left/right subtree attachment | `expr.c:1015-1074` |
| Expression-list propagation | `expr.c:858-888` |
| Every function constructor sets its own bit | `expr.c:1190-1210` |
| Child SELECT is a separate boundary | `expr.c:1076-1090` |
| COLLATE initializes rather than propagates ordinary child flags | ADR-0152/0162 and the canonical syntax kernel |
| Alias replacement copies the target, not its existing parents | `resolve.c:68-106` |
| Join-condition coalesce uses the function constructor | `select.c:597-639` |
| FULL/USING name replacement changes the opcode without setting the function bit | `resolve.c:764-779,836-866` |
| Native constant-expression generation consumes the fact | `expr.c:5895-5947` |

Modern already has `ExpressionHasLocalFunction(expression, child_presence)`,
the optional `SyntaxNormalization::has_local_function` annotation, the
canonical binding continuation driver, explicit-versus-syntax property
publication and scoped query/definition restoration. Reuse those owners;
do not add another syntax walk, annotation allocation or expression registry.

## Decision

Add `has_function_flag` to the existing bound-expression properties as a
native construction fact, alongside `has_subquery_flag`. It is not semantic
recursive function containment. Functions inside a child SELECT do not set
the containing query expression's bit, COLLATE remains a barrier, and
resolved alias children must not retroactively retag existing parents.

When the existing syntax annotation is present, consume its native fact.
For unannotated expressions, fold the same pure syntax kernel during the
existing binding continuation traversal. One boolean in the current/pending
binding frame accumulates original child-construction presence. Carry that
original fact back to the parent separately from the returned bound ID:
an identifier can resolve to a function-bearing alias while its original
identifier contribution remains false. Do not derive a parent's bit from
already resolved child properties or call the recursive whole-tree helper
once per bound node.

Actual alias/property copies retain the target's complete fact. Generated
native expression nodes derive it from their own already-resolved inputs,
not unrelated active syntax. In particular, generated AND/comparison/limit
nodes combine their real inputs and singleton-IN's generated unary node uses
its own operand. Distinguish the two existing joined-coalesce producers:
join-condition coalesce uses `sqlite3ExprFunction` and has its own function
bit; FULL/USING name replacement changes a name into `TK_FUNCTION` without
that constructor and does not invent the bit. Explicit names and wildcard
publication reuse that latter owner. A function-shaped bound payload alone
is therefore not sufficient evidence for setting the flag.
Explicit ORDER BY COLLATE wrappers clear it, just as ordinary COLLATE
construction does.

Extend the existing publication/conversion helpers and scoped binding/query
save/restore. Preserve the active construction fact on normal, typed-error,
allocation-failure and semantic reentry exits. No parser/catalogue/VM state
is moved across the dependency DAG, and no new cache or continuation owner
is introduced.

Use a named construction-fact value when an existing publication helper
needs the query/function/origin facts together, rather than adding adjacent
unrelated boolean arguments. This is a small nonowning argument record,
not another expression-state owner or property registry.

### Preserve the existing carrier size

The current public properties are16 bytes and bound expressions112 bytes.
An additional ordinary boolean would enlarge this frequently owned carrier.
Use adjacent standard C++23 one-bit boolean fields for the two construction
facts in the existing public and internal property records. Keep all other
field ordering and meanings intact. This is a bounded storage change for
two named facts, not a new mask API or compiler extension.

Repository inspection finds no address, member-pointer or mutable-reference
use of the existing construction boolean. Verify the actual MSVC/GNU carrier
sizes, strict compilation and all existing property users rather than
assuming a cross-compiler layout. Retain the original depth, stack and
allocation ceilings; do not weaken a failing ownership check to accommodate
the representation.

## Test-first and delivery

Start with an owning binder test specifying native flags for ordinary
unannotated functions and COLLATE barriers; record the genuine missing-field
compile failure before production changes. Then cover alias replacement
versus retained parents, child queries, constructor-normalized IN/AND,
generated joins/limits, source-owner moves and scoped restoration in the
existing binder owner.

Retain the complete fast tier, existing mixed/default-stack tests and
allocation/OOM populations. Run the actual change-aware strict/native/
sanitizer qualification and independent source review before a normal
ADR-0106 commit. Keep the existing accepted prefix and update the same draft
only after these gates pass.

The next behavioral source still requires an accepted generation-owner
design and focused runtime red. First/incoming VALUES, deferred constants,
retained source metadata, ordinary name/value phases, unary hexadecimal
limits and default extraction are not closed by this metadata foundation.
No new IN surface or later DAG node is admitted.
