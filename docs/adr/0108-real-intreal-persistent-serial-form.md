# ADR-0108: REAL and IntReal Persistent Serial Form

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

The reference audit's R2 finding isolates a persistent representation
difference in REAL-affinity writes. Public SQL values and exact binary64
results already agree, and both representations pass pinned SQLite's
`integrity_check`, but Modern does not reproduce SQLite's storage-stage
IntReal rules.

For a rowid table with no affected index, pinned SQLite 3.54.0 writes the
following REAL-affinity table serial types:

| Input | Pinned table serial type |
|---|---:|
| INTEGER `9007199254740993` | 7 |
| REAL `-9223372036854775808.0` | 7 |
| INTEGER `42` | 1 |
| REAL `42.0` | 1 |
| INTEGER `140737488355327` | 5 |
| INTEGER `140737488355328` | 7 |
| INTEGER `-140737488355328` | 5 |
| INTEGER `-140737488355329` | 7 |

The independent pinned record vector is:

```text
0b07070101050705070607
4340000000000000
c3e0000000000000
2a
2a
7fffffffffff
42e0000000000000
800000000000
c2e0000000000020
0020000000000001
3ff8000000000000
```

Its complete serial-type list is `[7,7,1,1,5,7,5,7,6,7]`; the ninth field is
an ordinary INTEGER control and the tenth is a fractional REAL control.
Pinned SQLite returns REAL public types and exact rounded binary64 values for
the first eight REAL-affinity fields.

That table-only vector is not a universal representation rule. Pinned SQLite
has three distinct write paths:

1. **Table record owns affinity.** When no index entry is generated from the
   new row, `OP_MakeRecord` carries the table affinity. An integer result is
   marked IntReal. Compact values retain integer serial types 1 through 5, 8,
   or 9; a value that would require integer serial type 6 converts to binary64
   and uses serial type 7.
2. **Shared new-row registers own affinity.** Before generating any affected
   index entry, SQLite applies the complete table affinity to the new-row
   register range. REAL affinity converts every numeric integer result to
   REAL. The table record and every affected new index record then copy those
   already-affinitized values without applying affinity again. Compact
   integral values therefore use serial type 7 in both records.
3. **Stored keys are reconstructed raw.** DELETE, the old side of UPDATE, and
   `CREATE INDEX` suppress read-time REAL affinity and copy the stored field
   representation. A compact INTEGER remains compact and a stored REAL
   remains serial type 7.

The path choice is observable and stable:

| Operation | Table REAL field | Persistent REAL index term |
|---|---|---|
| INSERT into a table with no index | IntReal compact/wide rule | not applicable |
| INSERT with an existing index | shared REAL-normalized value | same shared value |
| CREATE INDEX after earlier inserts | unchanged table record | raw stored value |
| UPDATE with no affected index | IntReal compact/wide rule | unchanged |
| UPDATE with any affected index | every new table column is pre-affinitized | affected new keys copy the shared values |
| DELETE or UPDATE old-key removal | deleted table record | raw stored value |

For example, INTEGER `42` in a REAL column is serial type 1 when inserted
before an index exists. A later `CREATE INDEX` copies serial type 1. A row
inserted after that index exists stores serial type 7 in both the table and
index. Mixed serial forms in one table and index are therefore valid native
history, not corruption.

The same distinction applies when an affected index does not contain the REAL
column. If any index entry is generated from an UPDATE's new row, SQLite
pre-affinitizes the complete table row before constructing both the table
record and affected index records. An UPDATE that changes no indexed column
does not take that path.

Storage REAL normalization also differs from ordinary Modern REAL affinity.
SQLite first tries integer affinity and then rematerializes an integer result
as REAL. This converts REAL `-0.0` to REAL `+0.0`. Table-record-owned affinity
instead retains compact integer zero, serial type 8. Neither persistent path
stores negative-zero bits for that input.

Modern currently uses one `ApplyStorageAffinity` helper for table records and
persistent index keys. It applies ordinary REAL affinity and then
`LosslessRowId`, producing ordinary `SqlValue::Integer` values. The canonical
record codec consequently selects serial type 6 for the two wide R2 cases.
Persistent index lowering then applies NUMERIC affinity after
`RealStorageAffinityInstruction`. `CREATE INDEX` population also applies
NUMERIC affinity instead of preserving the raw stored class. UPDATE rebuilds
every catalog index instead of distinguishing affected indexes, so it cannot
select SQLite's two new-row paths.

The existing abstractions provide the required ownership:

- raw mutation snapshots already retain decoded INTEGER versus REAL storage
  classes;
- mutation value registers already own the complete new row;
- `BoundUpdateAssignment` identifies the effective assigned columns and
  rowid;
- `BoundIndexMaintenance` identifies every physical index term;
- `ApplyAffinity` already implements canonical NUMERIC conversion, including
  signed-endpoint exclusion;
- `SelectRecordSerialType` is the one canonical implementation of SQLite's
  integer-width boundaries;
- `RealStorageAffinityInstruction` is the typed bytecode boundary for
  storage-stage REAL register normalization; and
- `BuildTableRecordInstruction` already owns table-record construction
  immediately before the canonical record codec.

Ordinary runtime `SqlValue`, function numeric typing, comparison affinity,
read-time REAL affinity, temporary records, automatic indexes, and rowid
coercion have different contracts. They must not acquire an IntReal state.

## Decision

### 1. Publish whether table-record affinity is owned or pre-applied

Add a typed affinity mode to `BuildTableRecordInstruction`:

- `kApplyColumnAffinity` means the table record owns storage affinity; and
- `kAffinityPreapplied` means the input register range already owns the
  complete table affinity and the record builder must copy it unchanged.

The default and every existing schema/ANALYZE call site remain
`kApplyColumnAffinity`. The bytecode verifier validates the enum and preserves
the existing cursor, register-range, initialization, and statement-kind
contracts.

`BuildTableRecordInstruction` continues to:

- substitute NULL for an INTEGER PRIMARY KEY alias;
- enforce NOT NULL after the selected affinity stage;
- use the cursor descriptor's schema format; and
- call the one canonical record codec.

It does not infer pre-application from the runtime `SqlValue` type because an
original REAL and an already-affinitized REAL are observably different for
compact integral values.

### 2. Model MakeRecord-owned IntReal without a runtime value type

Do not add `SqlValueType::kIntReal`, a second value container, an encoding-hint
side array, or a second record encoder.

For a REAL-affinity table field in `kApplyColumnAffinity` mode:

1. apply NUMERIC affinity through the canonical runtime helper;
2. leave NULL, BLOB, nonnumeric TEXT, fractional REAL, infinity, and REAL
   signed-int64 endpoints unchanged;
3. when the result is INTEGER, ask `SelectRecordSerialType` for its canonical
   serial type;
4. retain the INTEGER when the selected type is not 6; and
5. when the selected type is 6, convert the exact integer to binary64 and pass
   an ordinary REAL to the canonical record codec.

This transient normalization is the Modern equivalent of SQLite's
`MEM_IntReal`: compact values reach the codec as INTEGER, while wide values
reach it as REAL. INTEGER and numeric-TEXT inputs wider than binary64
precision round only when SQLite converts wide IntReal to REAL. REAL `-0.0`
becomes compact integer zero. Existing REAL signed-int64 endpoints remain
REAL because canonical NUMERIC affinity excludes them from integerization.

The helper returns a typed result and propagates the codec error instead of
assuming record options are valid. Non-REAL table affinities keep their
existing `ApplyAffinity` behavior.

The six-byte boundary is not duplicated in the VM. The record codec remains
the sole owner of integer serial-width selection.

### 3. Normalize the complete new row before affected index generation

`RealStorageAffinityInstruction` models SQLite's storage-stage `OP_Affinity`
for a REAL column:

1. apply canonical NUMERIC affinity;
2. convert an INTEGER result to binary64 REAL; and
3. leave every other result unchanged.

This differs intentionally from ordinary `ApplyAffinity(..., kReal)`: an
existing exact integral REAL is normalized through INTEGER first, so REAL
`-0.0` becomes REAL `+0.0`. Ordinary runtime REAL affinity remains unchanged.

Before building a new row that has at least one affected index, lowering
applies the complete table affinity in place to every new value register:

- REAL columns use `RealStorageAffinityInstruction`;
- other columns use the existing typed `ApplyAffinityInstruction`; and
- the subsequent `BuildTableRecordInstruction` uses
  `kAffinityPreapplied`.

Affected new index keys copy those same registers without another affinity
instruction. This preserves one owner for the new-row values and makes the
table and affected indexes share the exact normalized representation.

INSERT affects every existing index. UPDATE affects an index when an effective
assignment targets:

- the rowid or INTEGER PRIMARY KEY alias; or
- any catalog column used by that index.

Lowering computes that set once from the existing bound assignments and index
terms. It opens, checks, removes, and reinserts only those affected UPDATE
indexes. An UPDATE with no affected index leaves the new-row values
unmodified before `BuildTableRecordInstruction`, which uses
`kApplyColumnAffinity`.

This affected-index selection is required storage semantics, not an
independent optimizer. Partial and expression index mutations remain outside
the already-supported mutation domain.

### 4. Reconstruct old and newly created index keys from raw stored values

DELETE and the old side of UPDATE copy index terms directly from the raw
mutation snapshot. They apply no REAL or NUMERIC affinity before encoding the
key. Rowid terms continue to use the owned integer rowid register.

`CREATE INDEX` similarly copies each stored table field directly into the
index record and applies no column affinity. This preserves compact INTEGER,
wide REAL, signed endpoints, and mixed histories exactly as stored.

An ALTER TABLE trailing column can be physically absent while
`ReadFieldInstruction` returns its catalog default. That synthesized value is
not a raw stored field. `ReadFieldInstruction` therefore has an explicit
optional missing-value affinity: CREATE INDEX supplies the column affinity,
and the VM applies the table-owned IntReal rule only when the requested field
is physically missing. A physically present field bypasses that affinity even
when the same instruction has a missing-value contract.

New INSERT/affected-UPDATE index keys also apply no per-key affinity because
their complete source row has already been affinitized once.

Do not retain a trailing NUMERIC affinity in any of these persistent index
paths. Search-key affinity, comparison affinity, read-time public REAL
affinity, temporary sort keys, and automatic indexes are unchanged.

### 5. Leave the codec and public runtime contracts unchanged

`EncodeRecord`, `SelectRecordSerialType`, and the record decoder retain their
existing public contracts. They continue to encode the `SqlValue` presented
by their owner and do not infer column affinity.

Read-time `RealAffinityInstruction` continues to convert decoded INTEGER table
or covering-index fields to public REAL values. Ordinary `ApplyAffinity`,
`ApplyNumericType`, rowid conversion, scalar functions, comparisons, sorters,
top-N records, and relations are unchanged.

Modern does not currently support DML against WITHOUT ROWID tables. This ADR
does not add that feature or claim coverage for its primary-key record path.
Schema and ANALYZE records have no affected REAL-affinity contract and retain
their existing table-record mode.

### 6. Preserve exact path and boundary evidence

Focused evidence must inspect serial types and payload bits for:

- compact positive and negative IntReal values at the six-byte boundary;
- the first positive and negative values requiring eight integer bytes;
- INTEGER `9007199254740993` and numeric TEXT with the same spelling;
- integral REAL values and REAL signed-int64 endpoints;
- REAL `-0.0`, `+0.0`, fractional values, infinity, nonnumeric TEXT, BLOB,
  and NULL controls;
- ordinary INTEGER serial type 6 as a non-REAL control;
- INSERT before versus after index creation;
- `CREATE INDEX` from compact and wide stored rows;
- mixed pre-index and post-index history;
- UPDATE with no affected index versus UPDATE with any affected index;
- affected UPDATE old-key removal and new-key insertion; and
- DELETE raw-key removal.
- synthesized compact, wide, and negative-zero REAL defaults for physically
  missing ALTER TABLE columns, alongside a physically present control.

The pinned table-only golden record above is independent of Modern's
implementation. Additional pinned vectors cover:

- compact table/index serial type 1 before index creation;
- compact table/index serial type 7 after an index already exists;
- raw `CREATE INDEX` preservation;
- mixed serial forms in one index;
- affected and unaffected UPDATEs; and
- positive-zero payload bits after storage normalization; and
- `CREATE INDEX` over SQLite-created ALTER TABLE defaults.

Modern-written databases must pass pinned SQLite `integrity_check`, public
type/value checks, exact indexed and nonindexed result-bit checks, and
serial-form inspection. SQLite-written databases must remain readable and
mutable by Modern across the same compact, wide, endpoint, negative-zero, and
mixed-history cases.

### 7. Use test-first, bounded delivery

Before production changes:

1. add direct table-record tests for MakeRecord-owned compact/wide IntReal;
2. add direct storage-affinity tests for compact/wide values and negative
   zero;
3. add lowering tests for table-record affinity modes, one complete-row
   pre-affinity pass, affected-index selection, and raw old/CREATE INDEX keys;
4. add exact table and index serial-form interoperability cases; and
5. record the expected focused failures.

Then implement the smallest bytecode, VM, and lowering correction. Run the
focused tests, complete fast tier, pinned golden/interoperability cases,
selected write/read fuzz domain, isolated Debug/Release/sanitizer
configurations, and GCC 16 Debug/Release compilation. Persistent-format
behavior is not performance-sensitive, so existing read/write performance
contracts must remain within their accepted bounds rather than establishing a
new speed claim.

Obtain GPT-5.6 Sol design review before production and implementation/exact-tree
review before commit. Open the continuation as a draft after the accepted
commit. This slice does not complete the remaining node-43 prerequisites,
node 43, or any later node.

The initial design review rejected a uniform persistent-index REAL rule and
ordinary REAL affinity because SQLite's representation is path-dependent and
storage normalization canonicalizes negative zero. The revised three-path
model, affected-index definition, complete-row affinity ownership, and raw-key
reconstruction passed GPT-5.6 Sol follow-up review before production began.

## Consequences

Positive:

- table-owned affinity reproduces SQLite's compact/wide IntReal rule;
- indexed new rows share one exact table/index REAL-normalized value;
- old keys and `CREATE INDEX` preserve valid mixed storage histories;
- negative-zero behavior is stable across table and covering-index reads;
- ordinary runtime values and the canonical record codec remain unchanged;
  and
- the VM reuses the codec's serial-width decision instead of duplicating a
  persistent-format threshold.

Negative:

- `BuildTableRecordInstruction` gains an explicit affinity-ownership mode;
- UPDATE lowering must identify affected indexes instead of rebuilding every
  catalog index;
- table and index serial forms intentionally depend on write history, matching
  SQLite; and
- exact persistent tests must inspect both table and index records rather
  than relying only on public SQL values.

## Rejected alternatives

### Add an IntReal runtime value type

Rejected because IntReal is a transient record-construction flag, not a public
SQL storage class. Extending `SqlValue` would leak write-stage state into
functions, comparisons, sorters, and public rows.

### Add record-field encoding hints

Rejected because a parallel hint array creates a second field-ownership
contract and cannot by itself express SQLite's shared new-row affinity stage.

### Use one representation for every REAL index term

Rejected because native indexes preserve raw compact rows created before the
index and store REAL-normalized rows inserted after it exists. Mixed histories
are intentional.

### Always let the table record reapply affinity

Rejected because an indexed INSERT or affected UPDATE already affinitizes the
shared new row. Reapplying MakeRecord-owned IntReal would change compact REAL
values back to INTEGER and make table and index representations diverge.

### Hard-code the six-byte boundary in the VM

Rejected because `SelectRecordSerialType` already owns every schema-format and
integer-width decision.

### Convert every eight-byte INTEGER to REAL in the codec

Rejected because ordinary INTEGER values must retain serial type 6. Only a
REAL-affinity table field with MakeRecord-owned affinity has IntReal intent.

### Reapply NUMERIC affinity while reconstructing index keys

Rejected because it changes a stored serial type 7 integral REAL into serial
type 6 and destroys raw mixed-history reconstruction.

## Traceability

Node: `implement-advanced-sql`, prerequisite closure ordinal 3.

Contracts: ADR-0015, ADR-0018, ADR-0053, ADR-0100, ADR-0102, and ADR-0106.

Pinned source:

- `src/insert.c:130-221` — table affinity as either register affinity or a
  `MakeRecord` operand;
- `src/insert.c:2422-2463,2725-2732` — one complete new-row affinity pass
  before affected index entries and conditional table-record affinity;
- `src/vdbe.c:3525-3547` — storage REAL register normalization;
- `src/vdbe.c:3615-3640` — affinity application and IntReal marking in
  `OP_MakeRecord`;
- `src/vdbe.c:3699-3755` — integer-width selection and wide IntReal conversion
  to serial type 7;
- `src/vdbemem.c:802-827` — exact REAL-to-INTEGER affinity with signed-endpoint
  exclusion;
- `src/delete.c:1007-1019` — raw stored-value index-key reconstruction; and
- `src/build.c:3891-3899` — `CREATE INDEX` reuse of the raw-key path.
