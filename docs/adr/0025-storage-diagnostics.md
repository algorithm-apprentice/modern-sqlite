# ADR-0025: Read-Only Storage Diagnostics

- Status: Accepted
- Date: 2026-10-03

## Context

Nodes 15 through 17 can establish a stable read snapshot, decode SQLite
B-tree pages, traverse table and index trees, and read local or overflow
payloads. The storage-read milestone still needs an offline command that
explains a database's physical structure and reports whole-file corruption
before any mutable database operation is implemented.

Pinned SQLite 3.54.0 defines the relevant behavior across `src/prepare.c`,
`src/btreeInt.h`, `src/btree.c`, `src/pragma.c`, `src/vdbe.c`, and
`src/shell.c.in`:

- `src/prepare.c:230-264` defines `sqlite_schema` as the five-column table
  `(type, name, tbl_name, rootpage, sql)` rooted at page 1.
- `src/btreeInt.h:612-666` defines pointer-map page placement and the root,
  free, first-overflow, later-overflow, and B-tree-child entry types.
- `src/btree.c:10694-10735` tracks page references, rejects page zero and
  out-of-range references, and reports a second owner for a page.
- `src/btree.c:10740-10834` verifies pointer-map entries and exact freelist or
  overflow-chain lengths.
- `src/btree.c:10886-11170` checks B-tree page coverage, table-rowid ordering,
  overflow ownership, recursive children, equal leaf depth, duplicate byte
  use, and fragmented-byte accounting.
- `src/btree.c:11200-11347` checks the freelist, every supplied root, the
  auto-vacuum maximum-root header, pointer-map page ownership, and pages that
  are never referenced.
- `src/pragma.c:1677-1765` obtains all table and index roots from the loaded
  schema before invoking the B-tree integrity checker.
- `src/pragma.c:1767-2080` performs later schema-aware checks such as index
  entry counts, table-to-index correspondence, strict typing, and constraints.
- `src/vdbe.c:7435-7490` limits integrity errors and returns structural
  findings without converting every corrupt page into an operational failure.
- `src/shell.c.in:6112-6200` exposes deterministic database-header fields in
  the shell's `.dbinfo` command.

The production `ReadPager`, `BtreePageView`, `OverflowPageView`,
`FreelistTrunkView`, `PointerMapView`, and `RecordView` already own the format
and bounds checks needed by an inspector. Diagnostics must call those
decoders rather than introduce a second interpretation of SQLite bytes.

This node precedes the lexer, parser, and catalog model. It can discover root
pages from the fixed `sqlite_schema` record layout, but it must not add an ad
hoc SQL parser to reconstruct index expressions, collations, partial-index
predicates, generated columns, or table constraints.

## Decision

Add a read-only storage inspection library under
`modern_sqlite/diagnostics/storage_inspector.hpp` and an executable named
`modern_sqlite_inspect`.

The public entry points are:

```cpp
enum class StorageIssueCode : std::uint8_t {
  kHeaderInvariant,
  kSchemaRecord,
  kDuplicateRoot,
  kInvalidPageReference,
  kDuplicatePageReference,
  kPageDecode,
  kTreeKindMismatch,
  kEmptyChildPage,
  kDepthLimit,
  kUnequalChildDepth,
  kTableKeyOrder,
  kPageCoverage,
  kRecordDecode,
  kOverflowChain,
  kFreelist,
  kPointerMap,
  kUnusedPage,
};

enum class StoragePageRole : std::uint8_t {
  kUnreferenced,
  kBtree,
  kOverflow,
  kFreelistTrunk,
  kFreelistLeaf,
  kPointerMap,
  kLocking,
};

enum class StorageSchemaObjectKind : std::uint8_t {
  kUnknown,
  kTable,
  kIndex,
  kView,
  kTrigger,
};

struct StorageText {
  std::vector<std::byte> bytes;
  bool valid_utf8 = true;
};

struct StorageHeaderReport {
  std::uint32_t snapshot_page_count = 0;
  std::uint32_t header_page_count = 0;
  ByteCount page_size;
  ByteCount reserved_bytes;
  ByteCount usable_size;
  std::uint8_t write_version = 0;
  std::uint8_t read_version = 0;
  std::uint32_t file_change_counter = 0;
  PageNumber first_freelist_trunk;
  std::uint32_t freelist_page_count = 0;
  std::uint32_t schema_cookie = 0;
  std::uint32_t raw_schema_format = 0;
  RecordSchemaFormat effective_schema_format;
  std::int32_t suggested_cache_size = 0;
  PageNumber largest_root_page;
  std::uint32_t raw_text_encoding = 0;
  std::uint8_t effective_text_encoding = 1;
  std::uint32_t user_version = 0;
  std::uint32_t incremental_vacuum = 0;
  std::uint32_t application_id = 0;
  std::uint32_t version_valid_for = 0;
  std::uint32_t sqlite_version = 0;
  bool auto_vacuum = false;
};

struct StorageObjectReport {
  std::int64_t schema_rowid = 0;
  StorageSchemaObjectKind kind = StorageSchemaObjectKind::kUnknown;
  StorageText declared_type;
  StorageText name;
  StorageText table_name;
  std::uint32_t root_page = 0;
  bool sql_is_null = true;
  ByteCount sql_bytes;
};

struct StorageRecordReport {
  std::vector<SqlValueType> field_types;
};

struct StorageCellReport {
  std::size_t index = 0;
  std::optional<PageNumber> left_child;
  std::optional<std::int64_t> rowid;
  ByteCount payload_bytes;
  ByteCount local_payload_bytes;
  std::vector<PageNumber> overflow_pages;
  std::optional<StorageRecordReport> record;
};

struct StoragePageReport {
  PageNumber page_number;
  StoragePageRole role = StoragePageRole::kUnreferenced;
  std::optional<PageNumber> root_page;
  std::optional<PageNumber> parent_page;
  std::optional<BtreePageType> btree_type;
  std::optional<std::size_t> depth_from_root;
  std::optional<std::size_t> cell_count;
  std::optional<ByteCount> cell_content_offset;
  std::optional<ByteCount> fragmented_free_bytes;
  std::optional<ByteCount> free_bytes;
  std::vector<StorageCellReport> cells;
};

struct StorageIssue {
  StorageIssueCode code;
  std::optional<PageNumber> page;
  std::optional<std::size_t> cell;
  std::optional<PageNumber> related_page;
  std::string message;
};

struct StorageInspectionSummary {
  std::uint32_t snapshot_page_count = 0;
  std::size_t root_count = 0;
  std::size_t object_count = 0;
  std::size_t btree_pages = 0;
  std::size_t overflow_pages = 0;
  std::size_t freelist_trunk_pages = 0;
  std::size_t freelist_leaf_pages = 0;
  std::size_t pointer_map_pages = 0;
  std::size_t locking_pages = 0;
  std::size_t unreferenced_pages = 0;
  std::size_t cell_count = 0;
  std::size_t record_count = 0;
  std::size_t issue_count = 0;
};

struct StorageInspectionOptions {
  std::size_t max_issues = 100;
};

struct StorageInspectionReport {
  std::optional<StorageHeaderReport> header;
  std::vector<StorageObjectReport> objects;
  std::vector<StoragePageReport> pages;
  std::vector<StorageIssue> issues;
  StorageInspectionSummary summary;
  bool root_discovery_complete = true;
  bool issues_truncated = false;

  bool ok() const noexcept;
};

Result<StorageInspectionReport> InspectDatabase(
    Vfs& vfs, std::string_view path,
    StorageInspectionOptions options = {});

Result<std::string> RenderStorageInspectionJson(
    const StorageInspectionReport& report);
Result<std::string> RenderStorageInspectionErrorJson(ErrorCode code);
```

The stable string mappings are the lowercase enumerator names without the
`k` prefix: for example, `duplicate_page_reference`, `freelist_leaf`, and
`trigger`. `SqlValueType` and `BtreePageType` use their existing project names
with the same lowercase snake-case convention.

All optional report fields serialize as explicit JSON `null`; they are never
omitted. `StorageInspectionReport::ok()` is true exactly when `issues` is
empty and `issues_truncated` is false. Report records never expose borrowed
pager bytes.

`max_issues` must be nonzero. The default matches SQLite's 100-error integrity
limit. Once the limit is reached, traversal stops, `issues_truncated` becomes
true, and no success-shaped completion is reported.

### Snapshot and read-only contract

`InspectDatabase`:

1. opens a `ReadPager` through the caller's `Vfs`;
2. begins one read transaction;
3. performs the entire inspection in a nested scope;
4. destroys the inspector and every page pin before calling `EndRead`; and
5. returns only after `EndRead` succeeds.

The pager opens the main database read-only and rejects unsupported hot
rollback-journal or WAL states. Diagnostics never call `WriteAt`, `Truncate`,
`Sync`, `Delete`, or any mutable B-tree operation.

Failure to open or lock the database, establish the snapshot, read a required
page because of I/O, allocate required memory, or release the transaction is a
fatal `Result` error. Structural corruption found after the snapshot exists is
recorded in `StorageInspectionReport::issues` so the caller receives bounded,
machine-readable findings.

`InspectDatabase` and both renderers are explicit allocation boundaries. They
catch only `std::bad_alloc` and convert it to `kOutOfMemory`; they do not catch
unknown exceptions. The OOM error object has an empty, allocation-free message
representation so persistent allocation failure cannot escape while the
failure is being translated. If inspection and `EndRead` both fail, the
cleanup error takes precedence because the pager may still own a lock. The
inspection result is returned only after cleanup succeeds.

An empty database is a valid report with a null header, zero pages, no
objects, and no issues.

### Header policy

For a nonempty database, the report copies every field already exposed by
`DatabaseHeader`:

- snapshot and raw header page counts;
- page, reserved, and usable sizes;
- read and write versions;
- file-change and schema cookies;
- freelist head and count;
- schema format and suggested cache size;
- largest root page and incremental-vacuum flag;
- text encoding;
- user version, application ID, version-valid-for, and SQLite version.

`snapshot_page_count` is exactly `ReadPager::page_count()`. It is the trusted
header count when the pager accepts that count and otherwise the physical
page count. Diagnostics inspect pages 1 through this effective snapshot count
and deliberately do not reopen the raw file to expose trailing pages hidden
by a trusted smaller header count. `header_page_count` preserves the raw
field.

The raw schema-format value is preserved in the report. Raw value 0 maps to
effective format 1, matching SQLite. Raw values 1 through 4 map directly;
larger values return `kNotDatabase`.

The raw text-encoding value is also preserved. SQLite interprets the low two
bits for the main database and maps an effective value of 0 to UTF-8. The
inspector therefore treats raw values whose low two bits are 0 or 1 as
effective UTF-8. Effective values 2 and 3 return `kProtocol` until the text
layer has UTF-16 record decoding. The inspector does not reinterpret UTF-16
bytes as UTF-8 or emit partial schema results.

The report records whether auto-vacuum is active from the nonzero
largest-root header field. A nonzero incremental-vacuum flag with a zero
largest-root field is a `header_invariant` issue, matching SQLite's integrity
check.

### Stable root discovery

Page 1 is always inspected first as the `sqlite_schema` table root. Every
table leaf record must decode through `RecordView` and contain exactly five
fields with the schema-table shape:

1. non-NULL object type;
2. non-NULL object name;
3. non-NULL table name;
4. non-NULL root page; and
5. SQL text or NULL.

Pinned SQLite reads these rows through `sqlite3_exec`, which applies
`sqlite3_column_text` to every non-NULL value before `sqlite3InitCallback`.
Diagnostics apply the same logical coercion. Text and BLOB values preserve
their bytes, numeric values use the runtime's SQLite-compatible text
formatting, and the coerced root page must be an exact unsigned decimal
32-bit value.

Schema text fields are owned byte strings, not assumed-valid `std::string`
values. Type classification compares the raw bytes with the ASCII words
`table`, `index`, `view`, and `trigger`. Names and table names preserve every
byte. The SQL field records only NULL status and byte length; this node does
not copy or parse the complete DDL.

`StorageText::valid_utf8` is computed by strict UTF-8 validation. Valid text
serializes as `{"encoding":"utf8","value":"..."}`. Ill-formed text serializes
losslessly as lowercase hexadecimal,
`{"encoding":"hex","value":"80..."}`. Ill-formed UTF-8 alone is not
corruption because SQLite permits such identifier bytes.

The inspector records all schema objects. Nonzero roots are collected,
deduplicated, sorted, and then inspected. Page 1 is added as a synthetic
`sqlite_schema` root because it has no row describing itself.

Only observable root rules are enforced without parsing SQL:

- views and triggers require root page zero;
- indexes require a nonzero root whose observed storage is an index B-tree;
- tables may have root page zero and remain an unclassified zero-root table;
- nonzero table roots may use observed table or index B-tree storage; and
- unknown object types are invalid schema records.

Diagnostics never label a zero-root table as virtual or an index-storage table
as WITHOUT ROWID. A matrix violation, root outside the snapshot, invalid
schema-record shape, unknown type, or conflicting objects that claim the same
nonzero root produces a stable issue and marks `root_discovery_complete`
false.

When root discovery is incomplete, diagnostics still inspect every root that
was discovered safely, plus the freelist and pointer maps. They do not emit
`unused_page` findings or compare the maximum discovered root with the header,
because the root set is known to be partial. This follows SQLite's partial
integrity-check rule and avoids cascaded false positives.

No SQL text is parsed in this node. Declared schema kind and observed root
storage kind remain separate fields and concepts.

### Page ownership model

The inspector maintains one claim slot for every snapshot page. A claim
contains:

- the storage role;
- the owning root, when applicable;
- the immediate parent page, when applicable; and
- the expected pointer-map type and parent, when auto-vacuum is active.

The roles are:

- `btree`;
- `overflow`;
- `freelist_trunk`;
- `freelist_leaf`;
- `pointer_map`;
- `locking`;
- `unreferenced`.

Page zero, a page beyond the snapshot, and the locking page are rejected as
ordinary references. A second claim produces `duplicate_page_reference` and
the second traversal does not reinterpret the already-owned page. This
detects cycles, shared children, overflow reuse, freelist duplicates, and
cross-role ownership conflicts with one mechanism.

The pending-byte locking page is preclaimed when it lies in the file.
Pointer-map pages are preclaimed when auto-vacuum is active.

### Freelist validation

The freelist walk starts from the header's first trunk page and uses
`FreelistTrunkView` for every trunk:

- every trunk and leaf page is range-checked and claimed exactly once;
- trunk chains must terminate;
- every trunk's leaf count and leaf references must decode;
- the observed number of trunks plus leaves must equal the header count; and
- auto-vacuum databases require a `kFreePage` pointer-map entry with no
  parent for every freelist page.

A zero freelist head with a nonzero count, a nonzero head with an impossible
count, duplicate entries, malformed trunks, and exact-count mismatches are
reported as `freelist` issues.

### B-tree traversal and ordering

Every root is traversed recursively through `BtreePageView`, with the
20-frame limit established by SQLite and ADR-0024.

The traversal enforces:

- root and descendant page-kind consistency;
- an empty page only at a leaf root, except page 1's virtual interior root;
- nonzero, in-snapshot child references;
- one owner for every child;
- equal child depth for every interior page;
- strictly increasing rowids on table pages;
- table-child bounds `(previous separator, current separator]`, with the
  right-most child above the final separator;
- exact cell and freeblock coverage without overlapping byte ranges; and
- fragmented gaps equal to the page header's fragmented-byte count.

Table interior cells remain separators and do not produce record reports.
Index interior and leaf cells both produce record reports.

The page report includes page type, root and parent, depth, cell count,
cell-content offset, fragmented bytes, decoded free bytes, and cells in
pointer-array order.

Index-key ordering is explicitly not reconstructed from raw `CREATE INDEX`
SQL. Structural index pages and every encoded index record are validated, but
collation-, direction-, expression-, and partial-index-aware ordering waits
for the parser and catalog nodes. SQLite similarly separates core B-tree
integrity from later schema-aware index checks in `pragma.c`.

### Payload and record validation

Every payload-bearing cell records:

- rowid or left child when present;
- declared, local, and overflow byte counts;
- the exact overflow page sequence; and
- decoded record field count and field types.

Before allocating a complete payload, diagnostics calculate the required
overflow page count and reject sizes impossible for the current snapshot.
They then:

1. copy the local payload;
2. follow exactly the required number of overflow pages;
3. claim each overflow page;
4. decode it through `OverflowPageView`;
5. require every intermediate next link;
6. require the final used page's next link to be zero; and
7. require auto-vacuum pointer-map entries for first and later overflow pages.

The cursor layer deliberately ignores an unused final overflow link for a
bounded read, but whole-file diagnostics validate exact ownership and chain
length like SQLite's `checkList`.

The complete bytes are parsed through `RecordView`. Malformed records produce
`record_decode` issues. Diagnostics report record types, not complete field
values, so output remains bounded by structural metadata rather than user
payload size. Schema records copy only type, name, and table-name bytes plus
the SQL field's NULL status and byte length into object metadata.

### Pointer-map validation

When auto-vacuum is active, the inspector enumerates pointer-map pages through
`IsPointerMapPage` and decodes expected entries through `PointerMapView`.

Expected entries are:

- `kRootPage`, no parent, for non-page-1 roots;
- `kFreePage`, no parent, for freelist trunks and leaves;
- `kFirstOverflow`, owning B-tree page, for a cell's first overflow page;
- `kLaterOverflow`, previous overflow page, for later overflow pages; and
- `kBtreeChild`, parent B-tree page, for non-root tree pages.

Wrong types, wrong parents, invalid entry encodings, references to
pointer-map pages, and ordinary claims on pointer-map pages produce
`pointer_map` issues.

When root discovery is complete, the maximum nonzero root must equal the
header's largest-root field in an auto-vacuum database.

### Reachability and deterministic output

After freelist, root, overflow, pointer-map, and locking-page traversal,
complete inspections report every remaining snapshot page as
`unreferenced` and add one `unused_page` issue per page until the issue limit.

The report is finalized deterministically:

- schema objects sort by root page, raw declared-type bytes, raw name bytes,
  raw table-name bytes, SQL NULL status, SQL byte length, and schema rowid;
- pages sort numerically;
- cells remain in pointer-array order;
- overflow pages remain in chain order; and
- issues sort by page, cell, stable issue code, related page, and message.

No unordered-container iteration order reaches the public report.

`RenderStorageInspectionJson` emits UTF-8 JSON with a fixed key order and
correct escaping for quotes, reverse solidus, control bytes, and embedded
newlines. Every renderer-created message is project-controlled and contains
no source location, database path, `strerror` text, or platform-specific
wording.

The exact report key order is:

```json
{
  "format_version": 1,
  "ok": false,
  "root_discovery_complete": true,
  "issues_truncated": false,
  "header": {
    "snapshot_page_count": 0,
    "header_page_count": 0,
    "page_size": 0,
    "reserved_bytes": 0,
    "usable_size": 0,
    "write_version": 0,
    "read_version": 0,
    "file_change_counter": 0,
    "first_freelist_trunk": 0,
    "freelist_page_count": 0,
    "schema_cookie": 0,
    "raw_schema_format": 0,
    "effective_schema_format": 1,
    "suggested_cache_size": 0,
    "largest_root_page": 0,
    "raw_text_encoding": 0,
    "effective_text_encoding": 1,
    "user_version": 0,
    "incremental_vacuum": 0,
    "application_id": 0,
    "version_valid_for": 0,
    "sqlite_version": 0,
    "auto_vacuum": false
  },
  "objects": [
    {
      "schema_rowid": 1,
      "kind": "table",
      "declared_type": {"encoding": "utf8", "value": "table"},
      "name": {"encoding": "hex", "value": "80"},
      "table_name": {"encoding": "hex", "value": "80"},
      "root_page": 2,
      "sql": {"is_null": false, "bytes": 28}
    }
  ],
  "pages": [
    {
      "page_number": 1,
      "role": "btree",
      "root_page": 1,
      "parent_page": null,
      "btree_type": "leaf_table",
      "depth_from_root": 0,
      "cell_count": 1,
      "cell_content_offset": 430,
      "fragmented_free_bytes": 0,
      "free_bytes": 314,
      "cells": [
        {
          "index": 0,
          "left_child": null,
          "rowid": 1,
          "payload_bytes": 79,
          "local_payload_bytes": 79,
          "overflow_pages": [],
          "record": {
            "field_count": 5,
            "field_types": ["text", "text", "text", "integer", "text"]
          }
        }
      ]
    }
  ],
  "summary": {
    "snapshot_page_count": 2,
    "root_count": 2,
    "object_count": 1,
    "btree_pages": 2,
    "overflow_pages": 0,
    "freelist_trunk_pages": 0,
    "freelist_leaf_pages": 0,
    "pointer_map_pages": 0,
    "locking_pages": 0,
    "unreferenced_pages": 0,
    "cell_count": 2,
    "record_count": 2,
    "issue_count": 1
  },
  "issues": [
    {
      "code": "invalid_page_reference",
      "page": 2,
      "cell": 0,
      "related_page": 99,
      "message": "B-tree child is outside the snapshot"
    }
  ]
}
```

For an empty database, `header` is JSON `null`. Every optional page, cell,
and issue field is JSON `null` when absent. Byte counts and page numbers are
JSON unsigned integers. Record `field_count` is always
`field_types.size()`.

Fatal errors serialize only the stable typed code. Raw `Error::message()`
text is deliberately excluded:

```json
{
  "format_version": 1,
  "ok": false,
  "fatal_error": {
    "code": "not_database"
  }
}
```

Both renderers return `Result<std::string>` and convert `std::bad_alloc` to
`kOutOfMemory`.

### Command contract

The command accepts exactly one database path:

```text
modern_sqlite_inspect DATABASE
```

It writes one JSON document plus a trailing newline to standard output.
Usage errors write no standard output and one stable usage line to standard
error. Inspection failures write only fatal JSON to standard output.
On POSIX systems the command ignores `SIGPIPE` so a closed output pipe is
reported as exit code 1 rather than terminating by signal.

If normal rendering fails with `kOutOfMemory`, the command writes the fixed
allocation-free literal
`{"format_version":1,"ok":false,"fatal_error":{"code":"out_of_memory"}}`
through `std::fwrite`. Failure to write standard output is exit code 1 and is
not disguised as a successful inspection.

Exit codes are:

- `0`: inspection completed with no issues;
- `1`: usage or fatal operational/format error; and
- `2`: inspection completed and reported one or more structural issues.

The command performs no automatic recovery, journal deletion, WAL
checkpoint, repair, or mutation.

## Explicitly deferred behavior

- Parsing SQL text from `sqlite_schema`.
- Collation-aware index-key ordering.
- Table-to-index entry correspondence and exact index entry counts.
- CHECK, NOT NULL, affinity, STRICT-table, generated-column, foreign-key, and
  partial-index semantic validation.
- UTF-16 record decoding.
- Rollback-journal recovery, WAL snapshots, and shared-cache coordination.
- Repair, salvage, page rewriting, vacuuming, or mutation of any kind.
- Streaming JSON and bounded-memory inspection of multi-billion-page files.
- Performance gates beyond preventing accidental quadratic page ownership
  scans; this node is not performance-sensitive.

These checks belong to later parser, catalog, SQL execution, journal, WAL,
and hardening nodes.

## Validation plan

Tests use pinned SQLite 3.54.0 databases and focused byte mutations to cover:

- empty databases and exact database-header output;
- raw zero schema-format and text-encoding values, low-bit-compatible raw
  text encodings, unsupported effective UTF-16, and unsupported schema
  formats;
- trusted header page counts that intentionally hide appended trailing pages;
- schema root discovery for table, index, view, trigger, zero-root table, and
  table roots with observed table or index storage;
- ill-formed UTF-8 schema names preserved as hexadecimal JSON without a
  corruption issue;
- SQLite-compatible BLOB-to-text coercion for schema values;
- total object ordering when corrupt rows share root, type, and name fields;
- table and index B-tree page reports, records, local payloads, and overflow
  payloads;
- exact freelist traversal and count mismatches;
- auto-vacuum pointer-map roots, children, overflow pages, and free pages;
- duplicate page claims, cycles, invalid references, wrong tree kinds, empty
  children, excessive depth, unequal child depth, and rowid ordering;
- overlapping cells or freeblocks and fragmented-byte mismatches;
- premature, overlong, reused, and out-of-range overflow chains;
- malformed records and malformed `sqlite_schema` rows;
- unused-page reporting and suppression when root discovery is incomplete;
- issue-limit truncation and deterministic issue ordering;
- stable exact JSON for a golden database and fatal error;
- command exit codes and standard-output/standard-error separation; and
- a counting fake VFS proving that inspection performs no writes, truncates,
  syncs, or deletes;
- path-bearing POSIX failures rendered only as stable fatal error codes;
- allocation failure in inspection and rendering converted to
  `kOutOfMemory`; and
- unlock failure taking precedence after all page pins have been released.

The complete sanitizer, thread-sanitizer, static-analysis, formatting, graph,
fixture-hash, and whitespace matrix runs before review.

## Consequences

- The storage-read milestone ends with a user-visible offline command instead
  of requiring a SQL parser to understand physical corruption.
- Whole-file ownership, reachability, freelist, pointer-map, depth, ordering,
  record, and page-coverage checks stay outside cursor hot paths.
- Production decoders remain the single interpretation of SQLite bytes.
- Structural findings are bounded and machine-readable while operational
  failures remain typed errors.
- Schema-aware semantic integrity can be layered later without replacing the
  physical inspection report or command.
