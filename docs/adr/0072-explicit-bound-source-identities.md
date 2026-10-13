# ADR-0072: Explicit Bound Source Identities

- Status: Accepted
- Date: 2026-10-13

## Context

ADR-0053's next executable feature is multi-source binding and joins. The
aggregate packages are complete through PR #182. The current grammar still
admits zero or one table occurrence in each SELECT core.

The existing source contracts are insufficient for two occurrences of the same
table:

- `TableId` identifies a catalog object, not an occurrence or alias.
- Statement-global `BoundSourceColumnId` values distinguish ordinary columns,
  but their descriptors do not identify the owning source.
- `BoundRowIdExpression` has no owner at all.
- Logical and physical access leaves publish a query-core index but no source
  identity within that core.
- Binder and lowering each independently normalize aggregate rowid aliases.
  The optimizer repeats that normalization before converting columns into
  catalog-index coordinates.

The pinned local SQLite `src/resolve.c:250-265`, `503-512`, and `622-639`
assigns a resolved column the matched source occurrence's `iCursor`; hidden
rowid retains that occurrence and uses the rowid column marker. Modern must
preserve that distinction without importing runtime cursor IDs into binding.

This identity foundation is one coherent prerequisite PR. It does not accept
join syntax or expose SQL that the remaining compiler cannot execute.

## Decision

### 1. Identify occurrences independently from catalog objects

Add `BoundSourceId` through the existing `BoundId<Tag>` mechanism.

Each successfully bound FROM occurrence receives one dense statement-local
identity in source order. Different aliases and different compound arms retain
different identities even when their `TableId` and root page are equal.
Constant-row SELECT and VALUES cores consume no identity. Zero is the first
valid identity, not an absent-source sentinel.

`BoundTableSource` publishes that identity. `BoundSourceColumn` publishes its
owner while retaining the existing statement-global column ID and table-local
catalog `ColumnId`. `BoundRowIdExpression` explicitly publishes its owner.
Reactivating a bound core for clause resolution reuses its original identity
and never allocates another occurrence.

Existing single-target mutations use the first source identity for their
target, source columns, and rowid references. No new mutation source, syntax,
target container, or runtime capability is introduced.

### 2. Normalize source leaves once

Binding publishes a `rowid_alias` fact in each source-column descriptor.
Catalog-backed columns derive it from the owning table's accepted INTEGER
PRIMARY KEY alias; synthetic schema columns do not acquire an alias.

Add one shared `BoundSourceLeafIdentity`:

- the owning `BoundSourceId`; and
- an optional statement-global `BoundSourceColumnId`.

An omitted column denotes that source's rowid. Its explicit rowid and INTEGER
PRIMARY KEY column normalize to the same identity. A different source's rowid
never compares equal merely because both sources use the same table.

One allocation-free binder-level recognition helper handles direct column and
rowid expressions. Non-source expressions return no identity. It accepts
validated expression and column metadata from the same bound statement;
out-of-range column IDs violate the existing trusted bound-IR invariant and
must not silently become a nonmatch.

Binder aggregate equivalence/retention, optimizer rowid recognition and direct
grouping/ordering recognition, and lowering's retained-leaf mapping reuse this
normalization. Remove the duplicate binder/lowering identity containers and
algorithms. The optimizer may retain a narrow adapter from the canonical
statement-global column identity to a catalog-index `ColumnId`; this is a
coordinate conversion, not another rowid-normalization implementation.

Transparent aliases, parentheses, collations, affinity, and clause-specific
expression comparisons retain their existing rules outside this primitive.
The helper does not decide whether an expression is a statement invariant,
where it may be evaluated, or whether an access path supplies an order.

### 3. Carry ownership through the existing compiler

Logical scans and physical table, rowid, and index access leaves carry the
bound occurrence identity separately from their existing core index and
catalog IDs. Builders and shape validation preserve that identity.

Rowid and index eligibility must match the current occurrence, not just the
catalog table or a coincidentally equal local column number. Aggregate
equivalence and retained-row lookup include the owner.

Lowering consumes those semantic identities while retaining its current
single-source cursor lifecycle. It does not add a source registry to the VM,
change bytecode, duplicate the source-column array, or create a parallel
compatibility representation.

### 4. Preserve the SQL surface in this foundation

Parser source grammar, name-resolution visibility, wildcard expansion,
aggregate schedules, selected access behavior, and public SQL result metadata
remain unchanged. Comma/CROSS/INNER/LEFT execution and plural joined-output
publication follow in reviewed end-to-end PRs under ADR-0053.

No unexecutable multiple-source statement is accepted merely to demonstrate
the new identity type. No join reordering, automatic index, hash join,
subquery, coroutine, or outer-join null-row mechanism is preimplemented here.

## Verification

Start from a green SQL/compiler baseline. Focused ownership tests establish:

- same-table occurrences in separate compound arms have distinct identities;
- VALUES and constant-row cores do not consume an occurrence;
- ordinary columns, explicit rowid, and INTEGER PRIMARY KEY aliases publish
  the correct owner and canonical leaf identity;
- declared columns shadowing rowid retain ordinary-column identity;
- synthetic schema and catalog sources remain distinct;
- single-target mutation ownership remains consistent; and
- logical/physical access leaves preserve binder occurrence identity.

Run existing aggregate, compound, index, ordering, mutation, metadata,
reset/rebind, OOM/allocation, model, fuzz, and pinned differential coverage.
Retain exact allocation budgets unless a justified contract change is observed.
Use existing benchmark work contracts as non-target checks; this change
admits no performance tuning or new timing workload.

Review the source-ownership design and implementation before committing. The
dependency graph remains acyclic and `implement-advanced-sql` remains active.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5 prerequisite.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0046, ADR-0053,
ADR-0060, ADR-0061, and ADR-0068.
