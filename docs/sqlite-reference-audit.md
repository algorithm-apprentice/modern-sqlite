# Implemented-Engine SQLite Reference Audit

- Audit revision: `f9baa39e09e911003380ec7e93a23065222ceb7e`
  (tree `40ed5d19030cbd839c955f7e60b645510b37a4dc`).
- Reference: SQLite 3.54.0, Git
  `cb547ab3e931c7766e24834af5ef6c4578863e3d`, Fossil
  `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.
- Date: 2026-10-18.
- Status: ADR-0111's closed implemented-mechanism inventory is complete:
  90 production paths map to 36 mechanisms. Fourteen are reference-faithful,
  fifteen are accepted architectural translations, one is an accepted
  measured alternative, two require repair, and four remain unresolved
  pending matched profiles or preparation baselines.
  Feature expansion remains paused.
- Follow-up: Execute the ordered repair and resolution ledger below. Refresh
  every dependent inventory suffix after a lower mechanism changes, and do
  not resume node-43 feature work until no `repair-required` or `unresolved`
  entry remains.
- Traceability: `implement-advanced-sql`, ADR-0002, ADR-0045, ADR-0053,
  ADR-0111.

## Scope and interpretation

Compare the implemented engine, including completed prerequisites and admitted
advanced-SQL slices. Use ADR-0045's reference-faithful storage mechanisms,
observable SQL semantics, and Modern infrastructure distinctions. A different
class, container, physical plan, or opcode is not by itself a defect.

This is a bounded source-and-behavior audit, not a proof that every SQLite
input, crash, allocation boundary, or SQL composition is equivalent. Review
coverage and remaining uncertainty must be reported explicitly. Passing
existing tests does not discharge a newly reproduced discrepancy.

Do not count unimplemented features as admitted-surface bugs. In particular,
WAL, complete multi-connection locking, C API compatibility, extensions,
CTEs, views, triggers, derived-query sources, IN, window functions, CASE,
SQL CAST syntax, and the remaining constraint/conflict surfaces retain their
documented delivery boundaries. Scalar-value conversion primitives do not
imply that SQL CAST syntax has been delivered.

The parked B-tree header-reuse optimization and binder-foundation PR #203
are not part of the audit revision. The foundation has subsequently merged
after its own combined-tree review and gates. Neither is treated as the
sanitizer repair; that correction is A5.

## Method

1. Verify the native source identity and rebuild the exact audited Modern
   revision rather than reusing binaries from a different worktree state.
2. Review actual source mechanisms and cross-layer ownership/error seams,
   with native file/line references.
3. Reuse the existing profile-validated native oracle and Modern read-trace
   protocol. Compare rows, storage classes, exact REAL bits, metadata,
   primary error codes, failure phases, and lifecycle observations.
4. Generate independent bounded cross-feature probes without altering
   committed fixtures, old oracle records, frozen workloads, allocation
   limits, or deadlines.
5. Compare independent mutation traces, public transaction state, final
   native-readable contents, and `PRAGMA integrity_check`.
6. Separate matched behavior, explicit deferrals, confirmed root causes,
   unverified concerns, and performance differences.

The extended SQL probes use private SQLite-created fixtures and the existing
oracle profile, including UTF-8, database settings, compile-option checks,
source hashes, and limits. A second reproduction uses the unchanged joined
index fixture, avoiding dependence on a new diagnostic schema.

## ADR-0111 closed implemented-mechanism inventory

### Coverage population

The production population is closed over:

- 43 project-owned production translation units: the 42
  `MODERN_SQLITE_ENGINE_SOURCES` plus the instrumentation target;
- nine project-owned private production headers under `src/`;
- 38 public production headers under `include/modern_sqlite/`; and
- zero project-owned generated-source inputs at this revision.

Every one of the 90 paths appears in the coverage appendix below. Tests,
benchmarks, tools, CMake modules, and third-party sources are evidence or build
infrastructure rather than production mechanisms.

| Cluster | Paths | Mechanisms | Reference-faithful | Architectural translation | Measured alternative | Repair required | Unresolved |
|---|---:|---:|---:|---:|---:|---:|---:|
| Storage and transaction | 40 | 12 | 8 | 4 | 0 | 0 | 0 |
| Runtime and temporary storage | 12 | 7 | 5 | 0 | 1 | 0 | 1 |
| Text, syntax, catalog, and binding | 16 | 6 | 1 | 3 | 0 | 0 | 2 |
| Planning, lowering, bytecode, VM, and read session | 20 | 6 | 0 | 5 | 0 | 0 | 1 |
| Writable session and SQL mechanisms | 2 | 5 | 0 | 3 | 0 | 2 | 0 |
| **Total** | **90** | **36** | **14** | **15** | **1** | **2** | **4** |

### Mechanism dispositions

| Inventory entry | Modern mechanism | Pinned SQLite mechanism | Disposition |
|---|---|---|---|
| `storage-infrastructure` | Typed bytes, results, and optional counters | Narrow infrastructure equivalents in `sqliteInt.h` and `status.c` | `architectural-translation` |
| `storage-coding-format` | Coding, database header, and page-number contracts | `util.c`, `btree.c`, and `pager.c` format rules | `reference-faithful` |
| `storage-record-codec` | Shared record encoder/decoder and comparator | `vdbeaux.c`, `vdbemem.c`, and `vdbe.c` record rules | `reference-faithful` |
| `storage-vfs-posix` | Typed VFS and POSIX backend | `os.c` and `os_unix.c` | `architectural-translation` |
| `storage-page-cache` | Cache-local intrusive hash, unpublished fills, bounded clean-frame recycling, and sync-safe dirty selection | `pcache.c`/`pcache1.c` local hash, LRU, recycling, and `pSynced` dirty selection | `reference-faithful` |
| `storage-page-bitvec` | Shared adaptive page set | `bitvec.c` | `reference-faithful` |
| `storage-rollback-journal` | Main journal plus SQLite-shaped memory/spill statement journal | `pager.c` rollback state plus `memjournal.c` in-memory chunks and spill | `reference-faithful` |
| `storage-pager` | Typed read/write pager with page-local sync eligibility and freed-page suppression | `pager.c`, including `PGHDR_NEED_SYNC`, `PGHDR_DONT_WRITE`, per-cohort write authorization, and `pHasContent` | `reference-faithful` |
| `storage-btree-read` | Typed page views and read cursors | `btree.c` page/cursor mechanisms | `reference-faithful` |
| `storage-btree-write` | Writable cursor, bounded page owner, `CellArray`, and balancing | `btree.c` allocation, cell editing, balancing, insert, delete, and root lifecycle | `architectural-translation` |
| `storage-diagnostics` | Offline ownership and corruption workspace | `btree.c` integrity mechanisms | `architectural-translation` |
| `storage-transaction` | Compiler-gated anonymous savepoints and error-class-aware completion | `build.c` multi-write/abort flags plus `vdbe.c`, `vdbeaux.c`, and `btree.c` conditional statement transactions | `reference-faithful` |
| `runtime-values` | SQL values, affinity, numeric scanners, and exact comparison | `vdbemem.c` and SQLite floating conversion | `reference-faithful` |
| `runtime-collation` | BINARY, NOCASE, RTRIM, and custom descriptors | SQLite collation registry and comparators | `reference-faithful` |
| `runtime-functions` | Scalar and aggregate state registry | `func.c` plus `vdbe.c` function/AggStep invocation | `unresolved` |
| `runtime-temporary-core` | One shared relation Pager, one extent-managed sorter file, root reuse, and statement-wide poison | Per-cursor `BTREE_SINGLE` ephemeral B-trees and one `VdbeSorter` per sorter cursor | `measured-alternative` |
| `runtime-sorter` | Coallocated memory-mode records and one offset-linked file-mode bulk arena | `vdbesort.c` coallocated memory-mode records and file-mode bulk `SorterRecord` arena | `reference-faithful` |
| `runtime-topn` | Canonical ephemeral B-tree in memory and file modes | `select.c`/`vdbe.c` ephemeral B-tree ORDER BY LIMIT queue | `reference-faithful` |
| `runtime-ephemeral-relation` | Canonical ephemeral B-tree in memory and file modes | `select.c`/`vdbe.c` ephemeral B-tree membership and replacement | `reference-faithful` |
| `binding-text-lexer` | UTF-8/source text and tokenizer | `utf.c` and `tokenize.c` | `reference-faithful` |
| `binding-ast` | Immutable postorder syntax arena | Mutable `Expr`/`Select` ownership produced by `parse.y` | `architectural-translation` |
| `binding-parser` | Handwritten recursive descent plus Pratt expressions | Lemon `parse.y`/`parse.c` | `unresolved` |
| `binding-catalog-model` | Immutable dense catalog plus open-addressed lookups | SQLite Schema and Hash ownership | `architectural-translation` |
| `binding-catalog-loader` | Acyclic scan, parse, derive, and publish pipeline | `prepare.c`, `build.c`, and `analyze.c` schema initialization | `architectural-translation` |
| `binding-name-resolution` | Immutable bound arenas and lexical query owners | `resolve.c` NameContext walkers and `select.c` result processing | `unresolved` |
| `execution-logical-plan` | Immutable semantic query-block DAG | `select.c` semantic phases before code generation | `architectural-translation` |
| `execution-optimizer` | Native LogEst costs, prerequisite-bearing loops, pruning, and bounded N-best paths through immutable plan owners | `where.c` WhereLoop construction and path solving | `architectural-translation` |
| `execution-lowering` | Separate typed bytecode emission | `select.c`, `wherecode.c`, and `expr.c` schedules | `architectural-translation` |
| `execution-bytecode` | Immutable typed instructions and separate verifier | Mutable `VdbeOp` emission/finalization | `architectural-translation` |
| `execution-vm` | `std::variant`/`std::visit` dispatch with typed values and cursors | Compact `vdbe.c` opcode loop, `Mem`, and `VdbeCursor` | `unresolved` |
| `execution-read-session` | RAII prepare, bind, step, reset, and reprepare owner | `prepare.c`, `vdbeapi.c`, and `vdbeaux.c` lifecycle | `architectural-translation` |
| `write-session-lifecycle` | Writable RAII session over compiler, VM, and coordinator | SQLite write statement and connection lifecycle | `architectural-translation` |
| `write-dml-mutation` | Typed DML plans over one-pass cursors and index maintenance | `insert.c`, `update.c`, `delete.c`, and mutation opcodes | `architectural-translation` |
| `write-create-index` | Direct uniqueness probe plus B-tree insert for every source row | `build.c::sqlite3RefillIndex()` sorter and bulk ordered insertion | `repair-required` |
| `write-analyze` | Monolithic VM scan with payload copy and owned decode per row | `analyze.c` retained `StatAccum`, `stat_push`, and `stat_get` | `repair-required` |
| `write-schema-ddl` | Typed schema-row mutation, cookie update, and catalog refresh | `build.c`, `prepare.c`, and schema VDBE operations | `architectural-translation` |

### Per-mechanism source maps and evidence

Modern paths are listed in the closed coverage appendix. A pending repair has
no commit or PR identity yet; its stable identity is the blocking-ledger
ordinal below, and its child ADR is created before production changes.

| Inventory entry / DAG owner | Pinned SQLite functions and ranges | Prerequisites and current evidence | Disposition, uncertainty, and repair traceability |
|---|---|---|---|
| `storage-infrastructure` — `implement-bytes`, `implement-error-result`, `implement-instrumentation` | No source-identical database mechanism; narrow ownership/status analogues live in `sqliteInt.h` and `status.c`. | ADR-0004, ADR-0010-0012; ownership, allocation, compiler, and zero-cost instrumentation tests. | `architectural-translation`; no database-sized algorithm is replaced. |
| `storage-coding-format` — `implement-coding`, `implement-read-pager` | `util.c:1610-1750` varints; `btree.c:3312-3479` database-header and payload geometry. | Bytes/results, independent coding goldens, malformed headers, SQLite-created files. | `reference-faithful`; no open uncertainty. |
| `storage-record-codec` — `implement-record-codec` | `vdbeaux.c:3862-4190` serial types/get/put, `vdbeaux.c:4260-5130` record comparison, `vdbe.c:3600-3765` MakeRecord affinity/serialization. | SQL values/collations/coding; independent record goldens, malformed/OOM, prefix comparison, bidirectional images. | `reference-faithful`; ADR-0101 and ADR-0108 repairs are merged. |
| `storage-vfs-posix` — `implement-vfs-contracts`, `implement-posix-vfs` | `os_unix.c:1350-1575` deferred close, `:1672-2289` locks, `:3435-4035` positioned I/O/sync, `:6300-7218` open/access/delete. | Typed results/bytes; real-filesystem, short-I/O, locking, sync, recovery, and ADR-0097 evidence. | `architectural-translation`; WAL shared-memory ranges remain unimplemented node-45 scope. |
| `storage-page-cache` — `implement-page-cache` | `pcache.c:23-43,203-255,403-528,551-658` dirty lists, `pSynced`, fetch/release; `pcache1.c:117-210,537-641,876-1119` hash, LRU, recycling, rekey. | Byte ownership; cache unit/OOM/allocation tests, six isolated configurations, four fuzz domains, native contracts, and fresh matched read evidence. | `reference-faithful`; ADR-0112, implementation commit `f128e5915c95b4bcdfe637ce9edfcf16ec54fa99`, PR #222. ADR-0114 closes Pager publication and authorization. |
| `storage-page-bitvec` — `implement-journal-contracts`, `implement-write-pager` | `bitvec.c:15-541` bitmap, sparse hash, subdivision, recursive Set/Test/Clear. | Typed allocation errors; journal membership, savepoint suppression, `pHasContent`, OOM tests. | `reference-faithful`; ADR-0045 mechanism is merged. |
| `storage-rollback-journal` — `implement-rollback-journal` | `pager.c:1039-1079,1849-1871,2280-2410,2872-2995,4188-4641` savepoint/main playback; `memjournal.c:1-16,84-290,353-392` chunked memory journal and spill. | VFS/coding/journal contracts; crash, torn-write, hot recovery, metadata, savepoint, OOM, configuration propagation, version-2 matched baseline, and interoperability evidence. | `reference-faithful`; ADR-0113 and implementation commits `17fbd16`, `362c4f3`. Conditional journal admission remains ledger 8. |
| `storage-pager` — `implement-read-pager`, `implement-write-pager` | `pager.c:137-357` states, `:3075-3140` page reads, `:4714-4724` sync-safe spill, `:5597-5698` fetch, `:6050-6273` journal-before-write and sector propagation, `:6338-6352` DONT_WRITE, `:7240-7358` page-move sync transfer, `:6510-6799` commit; `pcache.c:445-470,645-658` sync-safe selection/flag clearing. | VFS/cache/journal; crash, savepoint, spill, sector, permutation, OOM/allocation, native contracts, and fresh version-2 write evidence. | `reference-faithful`; ADR-0114 and implementation commits `2aadeee`, `c145097`, `022e369`. |
| `storage-btree-read` — `implement-btree-page`, `implement-btree-read` | `btree.c:1205-2294` page/cell geometry, `:5155-5473` payload, `:5486-6520` cursor open/seek/traversal. | Pager/record/collation; goldens, malformed/fuzz, signed rowid/index seek, OOM and SQLite-created traversal. | `reference-faithful`; no open valid-file finding. |
| `storage-btree-write` — `implement-btree-write` | `btree.c:3540-3573,6546-7290,7320-9300,9441-10060,10098-10480` database/root, allocation/free, cell edit/balance, insert, delete, and root lifecycle. | Writable Pager and read B-tree; model, crash, OOM/allocation, bidirectional integrity. | `architectural-translation`; algorithm is preserved through typed owners, and ADR-0114 closes freed-page write suppression. |
| `storage-diagnostics` — `implement-storage-diagnostics` | `btree.c:10694-11310` integrity ownership/reachability plus format decoders above. | Read Pager/B-tree/record; whole-file corruption, freelist, overflow, pointer-map, stable output tests. | `architectural-translation`; offline workspace is outside point hot paths. |
| `storage-transaction` — `implement-transaction-coordinator` | `build.c:5484-5520` `isMultiWrite`/`mayAbort`; `vdbe.c:3959-4310` transaction/savepoint/conditional statement transaction; `vdbeaux.c:3220-3280,3320-3510` close/rollback; `btree.c:4615-4635` anonymous statement savepoint. | Pager/B-tree; compiler effects, scoped ANALYZE, lifecycle, crash, OOM, changes/rowid/autocommit, named savepoints, and matched write evidence. | `reference-faithful`; ADR-0119 and `5b304be` translate conditional admission and native error completion while retaining canonical epochs, savepoints, and terminal cleanup. |
| `runtime-values` — `implement-sql-value` | `vdbemem.c:330-885` numeric/affinity state, `vdbe.c:1851-2211` arithmetic/affinity, `vdbeaux.c:4260-5130` ordering, `util.c:1300-1750` decimal/varint helpers. | Text/results; differential conversion, exact mixed numeric comparison, NaN, embedded-NUL, R1/R2 evidence. | `reference-faithful`; no open semantic mechanism. |
| `runtime-collation` — `implement-collation` | `main.c:1082-1135` RTRIM/NOCASE, `main.c:2907-2965,3577-3586,3812-3868` registration/BINARY built-ins. | SQL values/text; allocation-free ordering, custom callback, record/index integration tests. | `reference-faithful`; UTF-16 adapters remain unimplemented scope. |
| `runtime-functions` — `implement-function-registry`, `implement-advanced-sql` | `func.c:1870-2340,3399-3550` aggregate/scalar states; `vdbe.c:7982-8180,9034-9098` AggStep/final/function invocation. | Values/collation/VM; full semantic, overflow, callback, reset and aggregate differential matrices. | `unresolved`; ledger 13. Profile must attribute the 6.32x global-functions gap before a child ADR. |
| `runtime-temporary-core` — `implement-advanced-sql` | `vdbe.c:4685-4734` one `BTREE_SINGLE` open per ephemeral cursor; `vdbesort.c:308-336,1081-1177` one sorter owner and memory policy per sorter cursor; `vdbe.c:987-990,9482-9520` and `vdbeaux.c:2805-2859,3326-3510` statement-wide cursor cleanup. | VFS/Pager/B-tree and statement budgets; ADR-0058 budget/file/root/extent/poison tests, consumer semantics, wide-modifier matched evidence, and 32-positioned-root cache diagnostics. | `measured-alternative`; ADR-0115 and diagnostic commit `537dfe2`. Explicit-transaction rollback differences remain ledger 8. |
| `runtime-sorter` — `implement-advanced-sql` | `vdbesort.c:1081-1177` mode setup, `:1944-2046` memory/file record admission, `:1725-1910` PMA spill, `:2517-2817` merge/rewind/next. | Temporary core/record/VFS; direct-insertion allocation, malformed/OOM, PMA, buffered-I/O, ordering, aggregate, fuzz, and cross-compiler evidence. | `reference-faithful`; ADR-0116 and implementation commits `704cf4a` and `60026ba` reproduce mode-specific record ownership, while `514143a` records the diagnostic allocation contract. |
| `runtime-topn` — `implement-advanced-sql` | `select.c:742-903` `pushOntoSorter()` LIMIT queue; `vdbe.c:4685-4750` ephemeral open, `:6058-6185` delete, `:6411-6475` last, `:6728-6787` insert, and `:6998-7065` prefix comparison. | Temporary core and ephemeral B-tree; memory/file mechanism, prefix comparison, direct maximum deletion, lazy drain, lifecycle/OOM, allocation, cross-compiler, sanitizer, fuzz, and fresh matched evidence. | `reference-faithful`; ADR-0118 and `cda0505` use the canonical B-tree in both modes, `edd6ef0` records the cross-library diagnostic contract, and `f1f3c36` closes queue-width and borrowed-prefix boundaries. |
| `runtime-ephemeral-relation` — `implement-advanced-sql` | `select.c:904-996,1198-1388,2983-3740` DISTINCT/compound destinations; `vdbe.c:4685-4750,5506-5568,6728-6848` ephemeral membership/insert/delete. | Temporary core, record comparator, B-tree; memory/file mechanism, page-limit, root-reuse, OOM, allocation, DISTINCT/compound, cross-compiler, sanitizer, fuzz, and fresh matched evidence. | `reference-faithful`; ADR-0117 and `f0cfe4d` use the canonical B-tree from first insertion and remove sorted-array admission. Commit `95139c9` closes POSIX temporary-path OOM cleanup, `3b37ce1` scopes historical/current aggregate ownership contracts, and `e2b191a` is the const-only quality correction. |
| `binding-text-lexer` — `implement-text`, `implement-lexer` | `tokenize.c:216-265,600-744` contextual tokens/prepare loop plus `utf.c` byte classes. | Bytes/text; token streams, offsets, comments, NUL, fallback keywords, malformed inputs. | `reference-faithful`; no open mechanism. |
| `binding-ast` — `implement-ast` | No source-identical ownership; `parse.y` semantic values and SQLite `Expr`/`Select` graphs define input semantics. | Text/results; immutable arena, reference, source-span, depth, destruction and OOM tests. | `architectural-translation`; explicit non-native infrastructure. |
| `binding-parser` — `implement-parser` | `parse.y:1-170,268-340,396-478,600-979,1070-1525,1622-1637`; generated `parse.c:2473-2511,6116+`; `tokenize.c:600-744`. | Lexer/AST; broad grammar/error differential and deep/wide limits. | `unresolved`; ledger 14. ADR-0028 is the alternative identity; matched parse/prepare evidence is missing. |
| `binding-catalog-model` — `implement-catalog-model` | `build.c:338-399,527-544,1549-1960,2437-2555,4020-4620`; `prepare.c:96-203`; `analyze.c:20-67,1523-1648`. | AST/values; immutable identity, lookup, constraints, indexes, statistics tests. | `architectural-translation`; no open hot-path evidence beyond prepare ledger 15. |
| `binding-catalog-loader` — `implement-catalog-loader` | `prepare.c:96-203,225-445`; `build.c:1040-1120,1549-1960,2437-2555,4020-4620`; `analyze.c:1523-1648,1942-2008`. | Catalog/parser/B-tree/record; SQLite-created schema, malformed rows, DDL identity, STAT1, reload tests. | `architectural-translation`; recursive prepare is intentionally split. |
| `binding-name-resolution` — `implement-binder`, `implement-advanced-sql` | `resolve.c:276-804,934-953,1073-1402,1508-1881,1968-2329`; `select.c:411-528,1838-1908`. | Parser/catalog/functions/collations; joins, aliases, compounds, aggregates, nested-query differential/OOM/allocation evidence. | `unresolved`; ledger 15. ADR-0033/0090-0092/0110 are semantic authorities; matched preparation evidence is missing. |
| `execution-logical-plan` — `implement-logical-plan`, `implement-advanced-sql` | `select.c:7709-7728,8371-8473,1224-1344,2573-2618`; `where.c:6948-6965`. | Binder/catalog; plan ownership, clause order, joins, compounds, aggregate and allocation tests. | `architectural-translation`; semantic phases only, no runtime resources. |
| `execution-optimizer` — `implement-basic-optimizer`, `implement-index-planning`, `implement-advanced-sql` | `where.c:3223-3655,4007-4305,4941-5023,5846-6278,6838-7130`; `wherecode.c:970-1008,1680-2245`. | Logical plan/catalog/statistics; native access/order/callback controls, search bounds, OOM, compiler/sanitizer/fuzz gates, and matched index evidence. | `architectural-translation`; ADR-0120/ledger 9 preserves native costs, loops, bounded paths, and predicate/key phases while separating immutable planning from emission. |
| `execution-lowering` — `implement-read-lowering`, `implement-dml-ddl`, `implement-advanced-sql` | `select.c:891-900,1193-1950,2573-2621,8369-9046`; `wherecode.c:1680-2734`; `expr.c:2378-2410,4686-5850`. | Physical plans/bytecode; exact evaluation schedules, callbacks, cursors, OOM/allocation and verifier tests. | `architectural-translation`; repairs consume this canonical emitter rather than adding another compiler. |
| `execution-bytecode` — `implement-bytecode` | `vdbeaux.c:271-363,598-658,876-975,1286-1355,2658-2890`; `vdbe.h:50-93`. | SQL values/results; builder/verifier dataflow, capability, malformed/OOM, size tests. | `architectural-translation`; runtime representation uncertainty is owned by `execution-vm`. |
| `execution-vm` — `implement-read-vm`, `implement-dml-ddl`, `implement-advanced-sql` | `vdbe.c:902-1040,1427-2211,2640-3520,4529-4860,5638-5715,6309-6710,7982-8180,9034-9098`; `vdbeapi.c:834-980`. | Bytecode/Pager/B-tree/functions/temp storage; complete semantic/lifecycle suites and all matched execution baselines. | `unresolved`; ledger 12. Profile identity precedes any compact-dispatch child ADR. |
| `execution-read-session` — `implement-read-session-api` | `main.c:1273-1408,3388-3770`; `prepare.c:703-1000`; `vdbeapi.c:105-174,980-1120,1695-2060`. | Parser/catalog/compiler/VM/POSIX; prepare/step/reset/finalize, schema change, allocation, differential lifecycle. | `architectural-translation`; no open lifecycle finding. |
| `write-session-lifecycle` — `implement-dml-ddl` | `prepare.c:703-1000`; `vdbeapi.c:105-174,834-1120`; `vdbeaux.c:3220-3510`; `main.c:1273-1408`. | Read session/transaction/VM; writable reset/rebind, changes, rowid, autocommit, rollback and schema-refresh traces. | `architectural-translation`; statement-journal admission remains ledger 8. |
| `write-dml-mutation` — `implement-dml-ddl`, `optimize-one-pass-mutations`, `implement-index-planning` | `insert.c:888-1700`; `update.c:296-950`; `delete.c:288-859`; `vdbe.c:3577-3725,5744-6135,7203-7250`. | Binder/plans/VM/transaction/B-tree; model, crash, fuzz, one-pass, index-maintenance and interoperability evidence. | `architectural-translation`; remaining baseline gaps are assigned to lower ledger entries. |
| `write-create-index` — `implement-index-planning` | `build.c:3851-3943` `sqlite3RefillIndex()` sorter, adjacent UNIQUE check, bulk cursor and ordered `IdxInsert`. | Sorter, writable B-tree, schema mutation; index compatibility and baseline evidence. | `repair-required`; ledger 10, child ADR pending after sorter closure. |
| `write-analyze` — `implement-index-planning` | `analyze.c:166-250,695-945,977-1510,1523-1648` STAT1 table, `StatAccum`, scan, encode/load. | Index cursor/record/VM/schema refresh; STAT1 differential and index baseline evidence. | `repair-required`; ledger 11, child ADR pending. |
| `write-schema-ddl` — `implement-dml-ddl`, `implement-index-planning` | `build.c:1265-1540,2716-2940,4020-4558`; `prepare.c:96-445`; schema opcodes in `vdbe.c:3577-3725,7203-7250`. | Catalog/transaction/B-tree; SQLite-readable schema rows, cookie/crash/no-op/reprepare tests. | `architectural-translation`; CREATE INDEX population is separately ledger 10. |

### Current matched performance evidence

Rows naming a baseline use aggregate wall ratios from committed immutable
baselines. Rows labeled “after ADR” use the retained implementation evidence
documented by that ADR rather than replacing the immutable baseline. Values
greater than one mean Modern is slower.

| Baseline | Largest material ratios | Source evidence |
|---|---|---|
| Read | 1.53x fit scan; 1.49x pressure scan | `benchmarks/read-baseline-v1/aggregate.json` |
| Write | 8.52x matched delete scan; 6.97x matched update scan; 5.56x matched explicit batch insert | `benchmarks/write-baseline-v2/aggregate.json` |
| Write after ADR-0114 | 5.73x matched explicit batch insert; 2.79x matched delete scan; 1.83x matched update scan | ADR-0114 implementation evidence |
| Write after ADR-0119 | 4.97x matched explicit batch insert; 2.82x matched delete scan; 1.84x matched update scan | ADR-0119 `5b304be` implementation evidence |
| ORDER BY | 7.68x file top-N; 6.44x memory top-N; 6.37x multi-spill | `benchmarks/order-by-baseline-v1/aggregate.json` |
| ORDER BY after ADR-0116 | 4.30x integer memory; 2.15x payload memory; 1.95x multi-spill; 1.90x minimal spill | ADR-0116 implementation evidence |
| ORDER BY after ADR-0118 | 7.80x file top-N; 5.89x memory top-N; file allocations fall from 624,675 to 488,132 | ADR-0118 `cda0505` implementation evidence |
| DISTINCT/compound | 26.55x high-cardinality memory DISTINCT; 17.58x file UNION replacement; 12.90x file keyed relation | `benchmarks/distinct-compound-baseline-v1/aggregate.json` |
| DISTINCT/compound after ADR-0117 | 17.24x DISTINCT ORDER/LIMIT; 16.18x file UNION replacement; 13.95x keyed UNION before file pressure; 10.18x VALUES UNION ALL; high-cardinality DISTINCT falls from 26.55x to 8.90x | ADR-0117 `f0cfe4d` implementation evidence |
| Aggregate/grouping | 7.56x DISTINCT state; 6.91x sorted file grouping; 6.90x unique local ordering; 6.32x global functions | `benchmarks/aggregate-baseline-v1/aggregate.json` |
| Aggregate after ADR-0116 | 6.12x wide file modifiers with 274,555 allocations; sorted file grouping remains 6.92x | ADR-0116 implementation evidence |
| Index | 6.41x CREATE INDEX; 5.40x unselective covering read; 3.67x noncovering range; 2.84x ANALYZE | `benchmarks/index-baseline-v1/aggregate.json` |
| Index after ADR-0120 | 6.80x CREATE INDEX; 5.62x unselective covering read; 3.29x noncovering range; 2.81x ANALYZE | ADR-0120 `cc442a2` matched evidence; no execution-parity claim |

The following fixed-work observations identify mechanism differences rather
than timing noise:

- historical version-1 explicit batch insert wrote 305,372,100 subjournal
  bytes through 74,481 file writes while reporting `temp_store=memory`;
  ADR-0113's truthful version-2 profiles both record zero subjournal opens,
  writes, and bytes;
- historical delete scan wrote 4,380 Modern database pages and issued 7,738
  journal sync calls; ADR-0114 reduces those counts to 20 and 18;
- historical matched update scan issued 7,722 journal sync calls and remained
  6.97x slower; ADR-0114 preserves the 4,371 database writes, reduces journal
  syncs to 18, and records a fresh 1.83x wall ratio;
- ADR-0117 moves high-cardinality memory DISTINCT from suffix-shifting sorted
  arrays to the canonical ephemeral B-tree, reducing its wall ratio from
  26.55x to 8.90x while keeping memory mode fileless;
- ADR-0116 reduces the 65,536-row memory sorter from 131,182 to 65,655
  allocations and the file-spill sorters from about 402,700 to about 262,400
  through mode-specific coallocation and bulk ownership;
- ADR-0118 replaces the memory top-N linked list with the canonical B-tree,
  reduces memory top-N from 6.41x to 5.89x, and avoids wrapper materialization
  for 60,000 file-mode OFFSET skips while reducing allocations by 136,543;
- unselective covering-index execution records 8,202 Modern versus 8,201
  SQLite VM instructions but remains 5.40x slower;
- CREATE INDEX performs 118,924 Modern allocations and 105,193 cache hits
  because it probes and inserts each source key directly instead of sorting
  and bulk-appending; and
- ANALYZE performs 25,079 Modern allocations for 12,288 input rows because it
  copies and fully decodes each index record.

### Blocking repair and resolution ledger

| Order | Inventory entry | Classification | Required closure |
|---:|---|---|---|
| 1 | `storage-page-cache` | **Complete** | ADR-0112/PR #222 adds cache-local hashing, unpublished fills, bounded frame recycling, atomic sync-state permutation, zero-allocation pressure fills, and fresh matched evidence. |
| 2 | `storage-rollback-journal` | **Complete** | ADR-0113 adds the SQLite `memjournal.c` in-memory statement-journal path, explicit spill policy, independent historical validation, and a truthful version-2 baseline. |
| 3 | `storage-pager` | **Complete** | ADR-0114 publishes and clears `NEED_SYNC`, preserves page-number sync state across moves, authorizes sync-safe pages from durable cohorts while newer cohorts remain dirty, and suppresses cached freed-leaf writes. |
| 4 | `runtime-temporary-core` | **Complete** | ADR-0115 retains statement-shared relation/sorter arenas as a measured alternative with explicit file, memory, cache-pin, failure, reset, and matched evidence. |
| 5 | `runtime-sorter` | **Complete** | ADR-0116 coallocates memory-mode metadata/payload, adds SQLite's offset-linked file-mode bulk arena, preallocates PMA metadata before I/O, and records focused allocation plus fresh ORDER BY/aggregate evidence. |
| 6 | `runtime-ephemeral-relation` | **Complete** | ADR-0117 uses the canonical ephemeral B-tree in memory and file modes, removes sorted-array insertion, and records page-limit, OOM, ownership, and fresh DISTINCT/compound evidence. |
| 7 | `runtime-topn` | **Complete** | ADR-0118 reuses the ephemeral B-tree for logarithmic ORDER BY LIMIT admission, positioned maximum deletion, lazy OFFSET drain, cross-library allocation contracts, and fresh matched evidence. |
| 8 | `storage-transaction` | **Complete** | ADR-0119 translates compiler multi-write/ABORT effects, conditional anonymous savepoints, and error-class-aware completion; matched batch-INSERT allocations fall from 1,106,646 to 677,649. |
| 9 | `execution-optimizer` | **Complete** | ADR-0120/PR #230 translates native LogEst costs, cached clause prerequisites, loop/pruning/path state, source schedules, and key/predicate phases for admitted accesses. Full local/matched and all nine hosted gates pass; squash `efe0522` owns the exact accepted tree. |
| 10 | `write-create-index` | Repair | ADR-0121 maps sorter-backed refill, adjacent UNIQUE checks, retained encoded append ownership, and native bulk balancing; design review precedes implementation. |
| 11 | `write-analyze` | Repair | Stream borrowed index fields through a retained STAT1 accumulator without per-row record ownership. |
| 12 | `execution-vm` | Resolve | Profile same-work covering-index, aggregate, and sorter controls; repair compact dispatch/register/cursor mechanisms if confirmed. |
| 13 | `runtime-functions` | Resolve | Attribute the global aggregate gap after the VM profile and retain or repair the aggregate hot path accordingly. |
| 14 | `binding-parser` | Resolve | Add parse-only and prepare-only matched short/wide/deep/malformed workloads for the Lemon alternative. |
| 15 | `binding-name-resolution` | Resolve | Profile wide/deep/repeated-copy preparation and classify binder arenas and replay work. |

This order is dependency-sensitive. A lower repair invalidates dependent
performance evidence. Each repair remains one documentation-first,
test-first coherent slice; no new node-43 feature begins between repairs.

### Closed path coverage

The following mapping accounts for every project-owned production path at the
audited revision.

| Inventory entry | Production paths |
|---|---|
| `storage-infrastructure` | `include/modern_sqlite/base/bytes.hpp`<br>`include/modern_sqlite/base/result.hpp`<br>`include/modern_sqlite/instrumentation/counters.hpp`<br>`src/base/bytes.cpp`<br>`src/base/result.cpp`<br>`src/instrumentation/counters.cpp` |
| `storage-coding-format` | `include/modern_sqlite/base/coding.hpp`<br>`include/modern_sqlite/storage/database_format.hpp`<br>`include/modern_sqlite/storage/page_number.hpp`<br>`src/base/coding.cpp`<br>`src/storage/database_format.cpp` |
| `storage-record-codec` | `include/modern_sqlite/format/record_codec.hpp`<br>`src/format/record_codec.cpp` |
| `storage-vfs-posix` | `include/modern_sqlite/platform/vfs.hpp`<br>`include/modern_sqlite/platform/posix_vfs.hpp`<br>`src/platform/vfs.cpp`<br>`src/platform/posix_vfs.cpp` |
| `storage-page-cache` | `include/modern_sqlite/storage/cache/page_cache.hpp`<br>`src/storage/cache/page_cache.cpp` |
| `storage-page-bitvec` | `include/modern_sqlite/storage/page_bitvec.hpp`<br>`src/storage/page_bitvec.cpp` |
| `storage-rollback-journal` | `include/modern_sqlite/storage/journal/journal.hpp`<br>`include/modern_sqlite/storage/journal/rollback_journal.hpp`<br>`src/storage/journal/journal.cpp`<br>`src/storage/journal/rollback_journal.cpp` |
| `storage-pager` | `include/modern_sqlite/pager/pager.hpp`<br>`src/pager/pager.cpp`<br>`src/pager/write_pager.cpp` |
| `storage-btree-read` | `include/modern_sqlite/storage/btree/page.hpp`<br>`include/modern_sqlite/storage/btree/cursor.hpp`<br>`src/storage/btree/page.cpp`<br>`src/storage/btree/cursor.cpp` |
| `storage-btree-write` | `include/modern_sqlite/storage/btree/writer.hpp`<br>`src/storage/btree/writer.cpp`<br>`src/storage/btree/writer_internal.hpp`<br>`src/storage/btree/writer_internal.cpp` |
| `storage-diagnostics` | `include/modern_sqlite/diagnostics/storage_inspector.hpp`<br>`src/diagnostics/storage_inspector.cpp` |
| `storage-transaction` | `include/modern_sqlite/transaction/transaction_coordinator.hpp`<br>`src/transaction/transaction_coordinator.cpp` |
| `runtime-values` | `include/modern_sqlite/runtime/sql_value.hpp`<br>`src/runtime/sql_value.cpp`<br>`src/runtime/sqlite_float.hpp`<br>`src/runtime/sqlite_float.cpp` |
| `runtime-collation` | `include/modern_sqlite/runtime/collation.hpp`<br>`src/runtime/collation.cpp` |
| `runtime-functions` | `include/modern_sqlite/runtime/function_registry.hpp`<br>`src/runtime/function_registry.cpp` |
| `runtime-temporary-core` | `include/modern_sqlite/temporary_storage/temporary_storage.hpp`<br>`src/temporary_storage/temporary_storage_internal.hpp`<br>`src/temporary_storage/temporary_storage.cpp` |
| `runtime-ephemeral-relation` | `src/temporary_storage/ephemeral_relation.cpp` |
| `binding-text-lexer` | `include/modern_sqlite/text/text.hpp`<br>`src/text/text.cpp`<br>`include/modern_sqlite/syntax/lexer.hpp`<br>`src/syntax/lexer.cpp`<br>`src/syntax/token_text.hpp` |
| `binding-ast` | `include/modern_sqlite/syntax/ast.hpp`<br>`src/syntax/ast.cpp` |
| `binding-parser` | `include/modern_sqlite/syntax/parser.hpp`<br>`src/syntax/parser.cpp` |
| `binding-catalog-model` | `include/modern_sqlite/catalog/catalog.hpp`<br>`src/catalog/catalog.cpp` |
| `binding-catalog-loader` | `include/modern_sqlite/catalog/catalog_loader.hpp`<br>`src/catalog/catalog_loader.cpp` |
| `binding-name-resolution` | `include/modern_sqlite/binder/bound_select.hpp`<br>`include/modern_sqlite/binder/bound_statement.hpp`<br>`src/binder/bound_select.cpp` |
| `execution-logical-plan` | `include/modern_sqlite/planner/logical_plan.hpp`<br>`src/planner/logical_plan.cpp` |
| `execution-optimizer` | `include/modern_sqlite/optimizer/physical_plan.hpp`<br>`src/optimizer/physical_plan.cpp` |
| `execution-lowering` | `include/modern_sqlite/lowering/plan_lowering.hpp`<br>`src/lowering/plan_lowering.cpp`<br>`src/lowering/plan_lowering_internal.hpp`<br>`src/lowering/global_aggregate_access.hpp`<br>`src/lowering/global_aggregate_access.cpp`<br>`src/lowering/grouped_aggregate_lowering.hpp`<br>`src/lowering/grouped_aggregate_lowering.cpp`<br>`src/lowering/aggregate_modifier_lowering.hpp`<br>`src/lowering/aggregate_modifier_lowering.cpp` |
| `execution-bytecode` | `include/modern_sqlite/bytecode/program.hpp`<br>`src/bytecode/program.cpp` |
| `execution-vm` | `include/modern_sqlite/vm/vm.hpp`<br>`src/vm/vm.cpp` |
| `execution-read-session` | `include/modern_sqlite/session/read_session.hpp`<br>`src/session/read_session.cpp`<br>`src/session/session_internal.hpp` |
| `write-session-lifecycle` | `include/modern_sqlite/session/write_session.hpp`<br>`src/session/write_session.cpp` |

`runtime-sorter`, `runtime-topn`, `write-create-index`, `write-analyze`,
`write-dml-mutation`, and `write-schema-ddl` are mechanisms implemented inside
paths already listed under their owning production files. The path population
is counted once; mechanism entries may overlap one path.

### Prior bounded source coverage

| Area | Reviewed seams and evidence | Disposition |
|---|---|---|
| Bytes, errors, instrumentation | Ownership/result contracts and existing unit/layering gates; no requirement to reproduce SQLite containers | Retain Modern infrastructure; allocation-boundary gaps below remain actionable. |
| Binary coding, database format, record codec | Varints, endian/checksum rules, header normalization, serial widths, signed values, NaN, prefix/index comparison, independent golden vectors | No new valid-record ordering finding; owning decode/error allocation paths need correction. |
| POSIX VFS, cache, PageBitvec | Short I/O, synchronization, delete ordering, stable pins, dirty/LRU state, rekey, adaptive membership and failure state | Zero-size access policy differs; no additional confirmed cache/bit-vector defect. |
| Journal, Pager, transaction coordinator | Publication/playback, hot recovery, savepoints, write ordering, error state, crash/seam evidence | Recovery metadata short reads and implicit-completion state are confirmed concerns. |
| B-tree pages, cursors, writer, diagnostics | Payload/overflow bounds, signed seek, interior traversal, pin lifetime, freelist/balance/root lifecycle, ownership/reachability, bidirectional mutation/crash evidence | No new valid-file balancing/data-loss finding; writable unused-link policy and cursor allocation boundaries differ. |
| Text, values, functions | Conversion, exact mixed comparison, scalar ties, aggregate state/overflow/compensation, collation and embedded NUL | Aggregate numeric TEXT classification and REAL storage-form rules differ. |
| Syntax, catalog, binding | Precedence and explicit boundaries, schema/default/index normalization, namespaces, aliases, captures, aggregate phases, independent SQL and mutation transcripts | Qualified wildcard, semantic query-copy, aggregate ORDER registration, and outer aggregate ownership differences are corrected; two explicit rollback-replay boundaries remain scoped. |
| Logical/physical planning and lowering | Source ownership/readiness, access admission/costs, range residual removal, ordering/group destinations, preservation/replay, conditional versus value evaluation | A1-A5 below; typed subroutines/correlated ownership are reused, not replaced. |
| Bytecode, VM, read session | Descriptor/dataflow/capability checks, register/cursor transitions, record-cache invalidation, bind/reset/finalize/reprepare | Direct-bytecode virtual-row/range state is corrected; no additional confirmed read-session lifecycle defect. |
| Temporary storage | Shared budgets, migration, buffered PMAs, stable run order, bounded fan-in, top-N, relation borrowing, poison/reset/cleanup | No confirmed additional sorter/relation/order/budget defect in the bounded review. |
| Writable SQL and public transaction state | DML snapshots, duplicate targets, constraints, index maintenance, schema/statistics, savepoint outcomes, 98 independent traces | Ordinary traces match; injected implicit commit failure exposes a separate completion seam. |

The prior bounded review used three independent GPT-5.6 Sol source reviewers. They read their
assigned production files in full or function by function and mapped actual
native mechanisms. Test inventories and relevant assertions were reviewed;
not every assertion or every native source line was re-audited. The main
agent directly reviewed SQL compilation seams and executed cross-feature
comparisons. Absence of a confirmed finding is not module-wide equivalence.

## Prior audit evidence

| Evidence | Result at the audit revision | Limitation |
|---|---|---|
| Complete Debug unit tier | 1,399 cases pass | Existing expectations can themselves encode a native divergence. |
| Remaining Debug tiers | 50 cases pass | Model, differential, CLI, platform, tool, and project coverage is finite. |
| Existing committed read corpora | 697 comparisons and 13 explicit unsupported boundaries pass | The read corpus has reached its 512-case limit; additional domains need a separate corpus rather than a relaxed limit. |
| Isolated native contract tier | 68 cases pass | Existing performance and compatibility inputs remain unchanged. |
| Initial extended SQL matrix | 1,963 transcripts: 1,831 exact matches, 85 expected unsupported cases, 47 admitted differences | Differences are test instances, not independent bug counts. |
| Ordinary nullable-index follow-up | 26 of 102 transcripts differ | All observed differences involve upper-only range NULL exclusion. |
| HAVING follow-up | 4 of 10 transcripts differ | These reduce to the same index-range root cause. |
| Qualified wildcard follow-up | 4 of 32 transcripts differ | Direct qualified columns remain physical; wildcard expansion differs. |
| Mixed three-source join chains | 384 row/group transcripts match | Bounded combinations of INNER/LEFT/RIGHT/FULL, equality/IS/arithmetic keys, NULL filters, and captures. |
| Conditional predicate follow-up | 12 of 20 transcripts differ | Conditional jumps and value-expression evaluation have different native schedules. |
| Independent mutation audit | 98 traces, 281 statements match | Covers selected DML, duplicate targets, constraints, indexed maintenance, schema operations, savepoints, public state, and final native integrity. It is not an exhaustive mutation/crash campaign. |

The extended campaigns contain 2,511 transcripts, including 85 explicit
unimplemented-syntax controls. Their 93 admitted differences are grouped by
root cause below. Related follow-up cases intentionally overlap; these are
not 93 independent defects.

## Confirmed findings

### D1. Short journal metadata can authorize destructive false recovery

Priority: **critical durability risk; correction takes precedence over A1**.
Status: **corrected by ADR-0095/PR #204**.

Source review shows that a successful short VFS read of a size-admitted
rollback header becomes end-of-journal rather than I/O failure.
`src/storage/journal/rollback_journal.cpp:105-112,296-321,371-403` collapses
the result into absent playback metadata or a finished stream.
`src/storage/journal/journal.cpp:731-767` can then finalize/delete the journal.
Short super-journal tail/name reads have the same interpretation gap at
`rollback_journal.cpp:138-172`.

If database pages were already written, the journal can be the only source
of the old image. A failed read must not delete it or publish a recovered
snapshot. Native `src/pager.c:1627-1674,2872-2924,3007-3039` propagates
header I/O failure and finalizes only after success, while deliberately
tolerating short page-record crash tails at `:2972-2995`.

The pre-implementation Pager regression dynamically observed false success,
journal deletion, snapshot publication, and failure to restore the old page.
First/later-header and super-journal metadata regressions also failed while
the short page-record control passed. ADR-0095's bounded correction now
passes those regressions, the compiler/sanitizer matrix, the 1,403-case fast
tier, 68 isolated native contracts, and 20,000 read plus 20,000 write
generated inputs. The 27 inherited work properties remain unchanged.
GPT-5.6 Sol approved the implementation and integrated exact tree. The
normally refreshed head passed all nine hosted checks, including the
unchanged original sanitizer model in 6.68 seconds under its ten-second
deadline, and merged as `f1ed8f52355687ecc0493215c34569cb02f0444a`.
Physically truncated files and complete-but-unpublished headers retain their
crash-tail behavior.

### D2. Implicit commit rollback leaves stale statement success

Priority: **high observable lifecycle correctness**.
Status: **corrected by ADR-0098/PR #207**.

If an implicit DML VM completes but commit fails before durability, the
coordinator can successfully roll back the transaction and invalidate its
statement token. `src/session/write_session.cpp:461-478,600-659` nevertheless
retains pending success/change/catalog state. `changes()` can remain the
previous successful count, and Reset retries completion with the stale
token rather than making the statement reusable.

Native `src/vdbeaux.c:3412-3453,3489-3498` clears changes on autocommit
failure and `src/vdbeapi.c:126-149` rewinds even when reset returns the prior
error. A fix must distinguish this rollback from a durable
`kWriterFinished` cleanup failure, whose existing retry is intentional.
An injected lock-failure reproduction confirms rollback with the inserted
row absent, stale `changes()==1`, and failed reset/re-Step with a stale
transaction token. The pinned native BUSY control reports zero changes,
returns the prior error from Reset, and then successfully re-executes the
same statement. Ordinary successful and constraint traces did not exercise
this seam.

ADR-0098 now discards pending completion only after authoritative token
retirement, preserving still-valid durable-cleanup retries. Phase-correct
public and allocation regressions failed before the correction and now pass.
The 27 focused cases and complete Debug suite pass. Seven paired native
lifecycle transcripts also match changes, rowid, autocommit, rows/schema,
reset/finalize errors, and actual reexecution; native accepts all final
Modern images and all fourteen integrity checks pass. All local gates and
nine first-head hosted checks passed, including the unchanged original model
in 6.33 seconds under its ten-second deadline. Merged commit
`27e8a29a9e26ebde29f962d8745ee6069690ef87` has the exact Sol-accepted
tree `24c7054492585d8a3c00c148197bd4b7e34154c3`.

### D3. Empty POSIX super-journal files have the wrong existence meaning

Priority: **high consequence, sidecar-dependent recovery difference**.
Status: **corrected by ADR-0097/PR #206**.

`src/platform/posix_vfs.cpp:1817-1844` reports a zero-size regular file as
existing. Native `src/os_unix.c:6867-6898` reports it absent for
`SQLITE_ACCESS_EXISTS`. A retained cold individual journal referencing an
empty super-journal placeholder can therefore replay committed data in
Modern when native skips playback.

Modern does not create attached-database super-journals yet, but recovering
SQLite-created trailers is an admitted ADR-0041 surface. Custom VFS
existence policies and production POSIX parity must not be conflated.
The production-POSIX/default-native-VFS probe confirms `true` versus `false`
for the same zero-byte regular file. The independent cold-journal/image
comparison also confirms the consequence: nonempty masters restore `[1]`
in both engines, missing masters retain `[1,2]` in both, and empty masters
restore `[1]` in Modern but retain `[1,2]` in native. Native reads the final
Modern image in each case.

ADR-0097 corrects only canonical POSIX `kExists`, preserving custom-VFS
policies and the existing recovery path. Focused platform and real-Pager
regressions failed before that change and now pass, including exact complete
recovery images, missing/nonempty controls, individual-journal finalization,
symlinks, nonregular files, and explicit unexpected errors. The independent
comparison now matches all three native outcomes with byte-identical final
images and native `integrity_check=ok`. The first hosted head also exposed
shared recovery-test directory collisions; atomic reservation in the existing
helper passed 240 two-worker executions across six configurations without
weakening scheduling or deadlines. The normally appended head passed all nine
hosted checks, including the original model in 2.77 seconds under its
ten-second deadline. Merged commit
`b6e5e29904e2a769ae13b9f9de2bc8e2f5566f3d` has the exact Sol-accepted
tree `fd6d37552664b798197fc01b3cc4b7b53d978de8`.

### A1. Upper-only index ranges admit SQL NULL keys

Priority: **high, silent wrong results**.
Status: **corrected by resumed ADR-0094/PR #208**.

Using the unchanged joined-index fixture:

```sql
SELECT count(*) FROM key_rows WHERE numeric_key<3;
```

SQLite returns `4`; Modern returns `7`. With `<=3`, SQLite returns `5`;
Modern returns `8`. The three extra rows have NULL numeric keys. A scalar
query ordered by the same key returns NULL instead of `1`, and a pushed
HAVING predicate emits an extra NULL group.

`src/optimizer/physical_plan.cpp:1436-1460` selects the range predicate, and
`:3385-3402` removes selected predicates from residual evaluation. However,
`src/lowering/plan_lowering_internal.hpp:85-105` models explicit start/end
bounds and equality prefixes without the implicit non-NULL boundary.

Pinned `src/wherecode.c:1943-2022,2074-2078,2180-2186` explicitly seeks past
initial NULLs or stops before terminal NULLs, depending on the physical index
order and traversal direction. Merely comparing against the upper bound does
not enforce SQL's NULL-rejecting range semantics.

Repair the canonical traversal/key-block mechanism for ordinary covering,
noncovering, grouped/global aggregate, private-query, and joined consumers.
Do not patch only HAVING or reintroduce a private comparator.

ADR-0094 now implements that shared borrowed NULL boundary and key emission.
The pre-production fixture red observed `4` versus `7`; all 35 new native
shard cases now pass without changing old records, fixtures, profiles, or
the 512-case cap. Current Debug passes 1,480 tests. An exact replay of all
2,511 frozen audit instances resolves 31 old differences and introduces no
new or changed-unresolved outcome; native replay is unchanged. All local and
nine first-head hosted gates passed, including the unchanged original model
in 4.18 seconds under its ten-second deadline. Merged commit
`b1d60e4dc7324f79aff3165c4830735e8f473e1b` has the exact Sol-accepted
tree `dab91619de35f297b28f81103d46a6171c1d86e9`.

### A2. Qualified left wildcards lose native RIGHT/FULL USING values

Priority: **high, wrong values and metadata**.
Status: **corrected by ADR-0099**, including exact wildcard name spelling.

For a RIGHT/FULL join with `USING(id)` or `USING(t)`, native expansion of
`a.*` can use the merged joined value, even though an explicitly written
`a.id` or `a.t` remains physical. Modern instead publishes the physical
left value for the qualified wildcard, losing unmatched-right keys and, for
NOCASE matches, retaining the wrong spelling. FULL also retains a declared
type where the native merged expression has none.

At the audited revision, `src/binder/bound_select.cpp:4426-4527` restricts
joined-value expansion to unqualified wildcards. Pinned
`src/select.c:6015-6027,6358-6379` deliberately omits a physical qualifier
when the source lies to the left of a RIGHT/FULL boundary and the name
participates in any later USING clause.

Repair wildcard expansion through the existing joined namespace and
coalesce ownership, without changing direct qualified-column resolution.
Recheck later USING boundaries and mixed chains, not only a two-source join.

The bounded follow-up has 197 native/current instances, including different
column spelling and affinity. Its 53 differences include later mixed
boundaries and a coupled unqualified-RIGHT wildcard name discrepancy.
Wildcard names must keep their owning declared spelling even when a resolved
RIGHT value has another source's metadata. The original four cases are not
the complete correction scope.

ADR-0099 now selects generated qualification through the current core's
existing source order and normalized USING links, then reuses canonical
name resolution and provenance checks. Explicit qualified columns remain
physical. The existing metadata builder keeps the wildcard owner's declared
spelling while deriving type, affinity, and collation from the resolved value.
All 197 exact follow-up instances now match native, resolving all 53 former
differences. The frozen 2,511-instance replay against the post-A1 baseline
resolves 12 further instances with no new or changed-unresolved outcome and
unchanged native contracts; these overlapping counts are not added together.

The permanent 48-case namespace shard includes 47 comparisons and one
explicit unsupported control, including a native prepare-error control for
an unrelated later same-name source. A small independent native metadata
fixture records unequal name spelling and declared types; old fixtures,
oracles, profiles, and the 512-case limit remain unchanged. Public and binder
regressions failed before implementation. Complete Debug passes 1,485 tests,
including source-read and real spill-write recovery and the expanded
exhaustive allocation sweeps under the unchanged 1,024-allocation ceiling.
GPT-5.6 Sol accepted the design and bounded implementation without blockers;
the integrated exact-tree and delivery gates remain mandatory.
The accepted exact tree `439285f23cf7c8030726bf692e64717c0e94e678`
passed every local gate and all nine first-head hosted checks, including the
unchanged original model in 4.58 seconds under its ten-second deadline.
PR #209 squash merged as `b692920e590e2f9eebd25dff7e775eb379fb84c4`;
merged main and only the confirmed merged branch were verified before cleanup.

### A3. Predicate contexts use eager value-expression evaluation

Priority: **medium, observable error-phase differences**.
Status: **conditional branch mechanism corrected by ADR-0104; one independent
rowid-range/access case remains**.

For example, all IDs in the diagnostic input are positive:

```sql
SELECT id
FROM audit_l
WHERE id>0 OR abs(-9223372036854775808)
ORDER BY id;
```

SQLite returns the rows; Modern raises integer overflow. Similar differences
occur in HAVING, NOT, and NULL-containing predicates.

`src/lowering/plan_lowering.cpp:5728-5738` first emits the complete expression
as a value. Its binary emitter at `:5499-5510` eagerly evaluates both operands
unless a constant truth hint applies. Native distinguishes eager value
emission from conditional `sqlite3ExprIfTrue`/`sqlite3ExprIfFalse` in
`src/expr.c:6224-6585`.

A repair must preserve SQL three-valued logic and the native error schedule
without turning ordinary SELECT value expressions into universally
short-circuiting expressions.

ADR-0104 adds one shared conditional branch emitter across current predicate
surfaces while retaining scalar value emission and existing optimizer
placement. The frozen matrix improves from 8/20 to 19/20 exact matches.
Its dedicated shard matches 50 pinned comparisons and retains one unsupported
CASE boundary. The remaining WHERE case uses pinned INTEGER PRIMARY KEY range
access while Modern scans; it remains a separately attributed access-planning
difference rather than being hidden by residual reordering.

The first two-jump implementation exceeded the existing 64-source program
bound and was rejected. Two composite conditions on the existing typed jump
instruction preserve that ceiling. Implementation review separately caught
grouped-key recomputation; the final branch entry reuses the retained
representative register and preserves nondeterministic callback counts.

ADR-0105 addresses the remaining access mechanism directly: a typed
single-source rowid range candidate, SQLite-compatible rowid seek conversion,
and the canonical table row loop. It does not reorder residuals or expand
joined/mutation access. The frozen predicate matrix is now 20/20 exact: the
empty `id<0` range stops before the earlier residual overflow, matching the
reference evaluation phase. The existing index-range shard now preserves all
35 prior records and adds 20 rowid access records, for 54 native comparisons
and one unchanged unsupported write boundary.

### A4. Joined ON readiness and access-key phases

Status: **corrected by ADR-0107**.

Priority: **medium correctness; prerequisite for computed-key admission**.

The original 80-case audit found 16 differences. The expanded count/row and
empty-side matrix found 38 differing transcripts. INNER source-only guards,
constant guards before empty inputs, function keys, and outer lookup misses
do not share one native evaluation phase.

The current producer emits a complete own-ON condition per physical
candidate at `src/lowering/plan_lowering.cpp:9601-9605,9641-9645`.
`ReadLoweringJoin.KeepsComputedJoinKeysInTheirExistingCandidatePhase` at
`tests/unit/lowering/plan_lowering_test.cpp:1985-1995` even expects an overflow
for a source-false guard where pinned SQLite returns count zero.

ADR-0081 correctly warns that determinism alone does not authorize hoisting,
but retaining a candidate scan is not itself proof of full native error
compatibility. Reproduce and correct the schedule through canonical
predicate/access contracts before admitting broader function keys. Do not
add a second guard collection.

At prerequisite-batch commit 1, the frozen 640-case matrix matches 600 and
differs on 40: 16 INNER/CROSS readiness cases, 12 preserved candidate-miss
cases, and 12 preserved computed-key cases. ADR-0107 replaces the
complete-ON candidate callback with one term-level placement schedule,
selected access-term consumption, and a bounded deterministic scalar rowid
key domain. The unchanged 128-row function-key miss executes 131,340 Modern
instructions versus 652 for the admitted arithmetic control.

The implementation now exact-matches all 640 cases. One canonical physical
placement list assigns pre-open and source-ready ON terms, selected ON access
terms are consumed once, and selected WHERE constraints retain their existing
filter recheck. The deterministic scalar rowid miss executes 780 instructions
with the same 768 comparisons and 13 pages as the arithmetic-positioned
shape, instead of the 131,340-instruction scan baseline. The pinned
conditional shard adds all 40 former differences while preserving every
inherited record, plus an explicit earlier-scalar/later-arithmetic key-order
case, conditional/subquery access-order barriers, and later-RIGHT/FULL
left-prefix fences.

### A5. Arithmetic joined keys retained quadratic candidate scans

Priority at audit: **high performance; unchanged sanitizer deadline blocker**.
Status: **corrected by ADR-0096/PR #205**.

At the audited revision, `b.id=a.id+?3` never reaches a joined point candidate
because
`src/optimizer/physical_plan.cpp:481-509` admits only value leaves and lazy
coalesces. Native computes the equality key and emits a rowid seek in
`src/wherecode.c:1743-1770`.

Result-verified growth probes at 32, 64, and 128 rows execute 18,079, 68,575,
and 267,871 Modern VM instructions. Native executes 1,300, 2,196, and 3,988
VM steps. These show different growth, not a cross-engine CPU-speed ratio.
The admitted zero-shift point control is near-linear.

The exact unchanged complete model executes 2,034,538 instructions with 12
source data-page reads, 27,121 cache hits, and 12 misses. Warm arithmetic
queries read zero data pages. The quadratic work therefore persists despite
source-page caching. These counters do not rule out metadata or temporary
file I/O.

The separately preserved compact header-reuse change reduces model CPU by
about 8%; it does not remove this algorithmic amplification or establish an
exact-head hosted sanitizer repair.

The separate computed-rowid correction now uses canonical typed probes and
ON readiness. The unchanged complete model executes 587,276 instructions;
32/64/128-row arithmetic growth executes 2,343/4,359/8,391, rather than the
quadratic baseline above. The native phase and cross-feature comparisons
gain no new differences.

Exact head `b1ddd3bf92cc442bf722e6c3565438c2c2f5d8e6` passed the original
hosted sanitizer model in 6.60 seconds under its unchanged ten-second
deadline on its first CI run. All nine checks passed, and PR #205 normally
squash merged as `75308c5160689864c6855f4715963b30bdb16acb`.
The separate general callback phases in A3/A4 and other audit findings are
not claimed resolved.

### R1. Aggregate numeric TEXT classification crosses embedded NUL

Priority: **high, wrong aggregate storage class and potential overflow phase**.
Status: **corrected by ADR-0100**.

Exact public traces independently reproduce both directions:

- `sum(?1)` with TEXT hex `31006532` (`1`, NUL, `e2`) is native INTEGER `1`
  but Modern REAL `1.0`.
- TEXT hex `393030373139393235343734303939320078`
  (`9007199254740992`, NUL, `x`) is native REAL but Modern INTEGER.

At the audited revision, `src/runtime/function_registry.cpp:248-307`
inspects the complete explicit TEXT view for exponent/decimal syntax before
ordinary affinity. Native
`src/vdbe.c:329-365,436-448` uses zero-terminated real parsing followed by
the exact integer-range rule or a successful full-length integer parse.
Simply truncating before NUL is not a complete correction. SUM/TOTAL/AVG
share this conversion and need one canonical helper plus overflow-latch
regressions.

The post-A2 bounded baseline compares 168 instances on the unchanged core
fixture and profile: 20 of 91 TEXT cases and six of 36 overflow/lifecycle
cases differ, while all 36 BLOB and five scalar controls match.

ADR-0100 now reuses the shared value scanner, full-length integer parser,
exactness predicates, and REAL conversion through one function numeric-type
operation. Ordinary affinity, CAST, arithmetic, and the numeric state remain
separate and unchanged. All 168 exact instances now match native, resolving
the 26 former differences with unchanged native outcomes. All 2,511 frozen
post-A2 cross-feature outcomes remain unchanged; that earlier population did
not reproduce these parameter-driven NUL cases.

The standard numeric-type shard adds 54 cases, including 53 comparisons and
one unsupported control. It covers both required prepare-control kinds,
types and exact REAL bits, full-length and 51-bit boundaries, error/reset
phases, grouped/modifier/compound/private consumers, and cross-type rebinding.
It reuses the unchanged core fixture, profile, protocol, and limits, and
retains every old corpus and oracle byte. The existing lower/BLOB-to-TEXT
path provides a source-free raw seed without new syntax or generator changes.

Focused aggregate and public-session regressions failed before production
changes. Current Debug passes 1,492 tests with every inherited name retained.
The focused 25 cases and complete 1,432-case fast unit tier pass. The existing
allocation helper retains its original numeric case and exact two-allocation
contract, adding long NUL TEXT paths and successful same-state reset/reuse
after each injected failure. GPT-5.6 Sol accepted design and implementation
without blockers; integrated exact-tree and delivery gates remain mandatory.
The exact accepted tree `61eff1a3246c60b086285f031ceba09e72759271`
passed all unchanged-input local gates and nine first-head hosted checks,
including the original model in 7.27 seconds under its ten-second deadline.
PR #210 squash merged as `ceeb3e8006afad3322698a93ff7022e619b1f06e`;
merged main, parent, trailer, and named merged-branch cleanup were verified.

### R2. REAL storage conversion loses native IntReal serial-form rules

Priority: **corrected by ADR-0108**.

The original mechanism reused REAL affinity plus lossless rowid conversion.
INTEGER `9007199254740993`, REAL `-9223372036854775808.0`, and compact
INTEGER `42` under REAL table affinity therefore produced Modern serial codes
`[6,6,1]`, versus pinned SQLite `[7,7,1]`.

The completed audit found three native paths rather than one uniform REAL
rule:

- table-owned `MakeRecord` affinity retains compact IntReal serial types and
  converts only integer serial type 6 to REAL;
- INSERT or UPDATE with an affected index pre-affinitizes the complete new row,
  so the table and affected new index records share REAL-normalized values; and
- DELETE, old UPDATE keys, and `CREATE INDEX` reconstruct raw stored values,
  preserving valid mixed histories.

ADR-0108 adds explicit table-record affinity ownership, reuses
`SelectRecordSerialType` for the canonical six-byte boundary, normalizes
storage REAL through NUMERIC then REAL to canonicalize `-0.0`, maintains only
affected UPDATE indexes, and preserves raw old/index-build keys. Ordinary
runtime `SqlValue`, comparison affinity, read-time REAL affinity, and the
record codec contract remain unchanged.

Focused table/index tests cover compact and wide boundaries, signed endpoints,
numeric TEXT, negative zero, index-before/after insertion, mixed histories,
affected and unaffected UPDATE, DELETE, raw physical `CREATE INDEX` fields,
and synthesized compact/wide/negative-zero ALTER TABLE defaults. The
independent pinned golden serial list is `[7,7,1,1,5,7,5,7,6,7]`.
Modern-written and SQLite-written mixed histories pass pinned
`integrity_check`, exact public REAL-bit checks, and indexed/nonindexed
mutation checks.

### B1. Codec and read-cursor allocation boundaries leak exceptions

Priority: **medium, typed-error contract violations**.
Status: **corrected by ADR-0101**.

At the audited revision, record materialization/error construction at
`src/format/record_codec.cpp:722-726,824-836` and public read-cursor
Open/CopyPayload paths at
`src/storage/btree/cursor.cpp:1004-1009,1093-1108,1121-1157,1258-1273`
allocate outside a translating boundary. A failed vector, payload, metadata,
or implementation allocation can escape as `std::bad_alloc` rather than the
documented typed out-of-memory result.

Existing allocation-free comparison tests do not inject failures into those
owning operations. The requested GPT-5.6 Sol retry now independently
reproduces escaping `std::bad_alloc` for field-error construction, result
vector allocation, owned TEXT/BLOB materialization, both cursor-open
allocation stages, and both payload-copy APIs against the frozen baseline.
These probes do not replace exhaustive cleanup/pin-release and overflow-copy
regressions in the eventual correction.
Contain only expected allocation exceptions, reuse `Error::OutOfMemory`,
and do not add broad catches or success-shaped defaults.

The main-agent current-tree reproduction after PR #210 confirms all nine
named allocation sites still throw. The existing scoped diagnostic was
recompiled against that exact library rather than reused as a stale binary.
The inventory also identifies coupled page/cursor diagnostic constructors
that own strings before a protected boundary. ADR-0101 reuses the codec's
nonthrowing error pattern, explicit ownership boundaries, and the existing
module allocation helpers; it does not broaden allocator handling in the
optimizer/lowering or change persistent validation.
Design review additionally identified 15 page-local lvalue error forwards
that can copy owning diagnostics after the factory. The correction requires
move propagation from non-const Results and nested failure-index coverage,
not only a fail-index-zero factory test.

ADR-0101 now contains expected allocation failures through the existing
nonthrowing error pattern and ownership boundaries. All 15 page errors move
from non-const Results. The shared cursor payload-copy operation retains one
exact buffer, feasibility-before-allocation, the same walker, and unchanged
position/borrows on failure. Public API shape and persistent behavior remain
unchanged.

The existing allocation helpers observed the field and three mixed-record
escapes plus 38 overlapping cursor/page observations before production
changes. Their expanded green controls exhaust ownership and nested
diagnostic allocations, include indices after zero, verify cleanup and
same-input/Pager retry, and prove failed-Open pin release through `EndRead`.
Local and overflow copies cover both cursor types. A real third-read I/O
failure after copy allocation preserves the original error, cursor position,
local bytes, and successful exact retry.

The focused and complete 1,432-case fast tiers passed before and after the
green-only copy consolidation. Current Debug passes 1,493 tests, retaining
every inherited name plus one I/O regression. All 2,511 frozen post-R1 native
and Modern outcomes remain unchanged. Old comparison repetitions, allocation
ceilings, fixtures, oracles, profiles, workloads, and resource controls are
unchanged. GPT-5.6 Sol accepted the amended design and implementation without
remaining blockers; integrated exact-tree and delivery gates remain mandatory.
All unchanged-input local conditions and nine first-head hosted checks
passed on the accepted tree `67bee264a726fbf8d3254b4933e21409f2a3e054`.
The original unchanged model passed in 2.52 seconds under its ten-second
deadline. PR #211 merged as `fd693a8d45d192e92ed888670d3e76e7a980dbbf`;
main, parent, trailer, and named merged-branch cleanup were verified.

### B2. Writable payload reads consume an unused overflow link

Priority: **medium, malformed-image point-operation parity**.

`src/storage/btree/writer_internal.cpp:5163-5221` validates the final
overflow page's next link even after all logical payload bytes fit on that
page. Native `src/btree.c:5338-5345` and Modern's read cursor at
`src/storage/btree/cursor.cpp:458-478` do not consume that unused link.

This is not evidence that Modern writes invalid chains. A correction must
keep invalid consumed intermediate links as corruption and keep whole-file
diagnostics strict. The source-derived writable/read/native counterexample
has now been executed against the frozen baseline and exact pinned native
runtime: native and Modern read accept the unused final link, while Modern
writable lookup rejects it. All paths reject the consumed intermediate-link
control. No valid-image or Modern-write corruption claim follows from this
malformed-image point-operation difference.

ADR-0103 corrects the canonical writable payload reader by decoding the next
link only when more logical payload bytes remain. Public encoded-index and
table-mutation operations cover one and multiple overflow pages, exact and
missing keys, repeated access, typed allocation and I/O failures, rollback
state, and consumed-link controls. The pinned runtime accepts all five tested
unused final values and rejects the consumed invalid control. Strict decoding
and whole-file diagnostics remain unchanged.

## Direct-bytecode verifier/runtime finding

Status: **corrected by ADR-0109**.

The verifier previously published positioned/readable state to both
`CheckIndexRangeInstruction` edges. Runtime clears logical NULL publication
on the range-end edge, so an executable
`NullRow -> CheckIndexRange -> ReadField` program verified and then failed
because the B-tree cursor was not positioned.

ADR-0109 keeps ordinary NULL rows and range fallthrough readable, but publishes
open/unpositioned state to `end_target`. Covering range exhaustion now uses the
existing unpositioned completion label; noncovering first/subsequent ends use
their state-compatible first-end and index-exhausted labels; positioned LIMIT
completion remains separate. Direct bytecode rejects range-end reads, accepts
seek-miss/range-end unpositioned merges, and preserves the no-range NULL-row
control.

## Historical correction closure and current gate

Every historical D1-D3, A1-A5, R1-R2, B1-B2, and direct-bytecode finding in
this document has its recorded merged correction. ADR-0092's query-copy and
aggregate-ordering work and ADR-0110's outer aggregate ownership correction
subsequently merged through PR #220. The accepted PR #220 head and squash
merge share tree `40ed5d19030cbd839c955f7e60b645510b37a4dc`.

That closure does not imply whole-engine mechanism parity. The ADR-0111
inventory above supersedes the old prospective ordering and is now the
blocking work ledger. Its remaining three repairs and four resolutions must
close in dependency order before new advanced-SQL feature slices resume.

Preserve each source-derived versus executed distinction as repairs proceed.
No production claim of universal input equivalence follows from the inventory.

The audit does not advance the DAG, complete node 43, authorize a blind
sanitizer retry, or weaken any existing gate. Every correction remains a
test-first, reviewed, independently validated coherent slice.
