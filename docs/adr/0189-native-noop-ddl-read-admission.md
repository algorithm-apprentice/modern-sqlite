# ADR-0189: Native No-Op DDL Read Admission

- Status: Accepted
- Date: 2026-11-01
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `3800c9d`, ADR-0179, ADR-0182 and ADR-0188
- Related contracts: ADR-0048, ADR-0102, ADR-0106 and ADR-0111

## Completed source and owner mapping

PR #281 closes actual old-program admission, guard expiration and bounded
halt-before-reprepare on main `3800c9d`, tree `3ef4a035`, ledger160.
No-op CREATE TABLE/INDEX access was explicitly excluded from that correction.
Node43 remains incomplete. The remaining literal/name/unary prerequisites
still precede IN query-RHS admission.

The pinned SQLite source distinguishes three facts:

| Fact | Pinned source and owner |
|---|---|
| An existing CREATE target requires schema validation | `build.c:1347-1351,4170-4175` calls `sqlite3CodeVerifySchema` |
| No write transaction is requested | `build.c:200-225` emits `OP_Transaction` from separate cookie/write masks; no-op never sets the write mask |
| No-op DDL is not a read-only statement | `build.c:1233-1247` emits a journal-mode query to force `readOnly=0`; `vdbeaux.c:883-916` derives `readOnly` and `bIsReader` separately |
| The journal-mode query does not change the mode | `vdbe.c`, `OP_JournalMode`, preserves the current mode for `PAGER_JOURNALMODE_QUERY` |
| Actual read entry still validates the old guard | `vdbe.c:4245-4350` |
| Interrupt cleanup depends on read-only classification, not lock access | `vdbeaux.c:3335-3398`; NOMEM/FULL and I/O retain their separate full-rollback rules |
| Replacement owns fresh access and classification | `vdbeapi.c:980-1027`; `prepare.c:907-934` |

The local probe links the unchanged ordinary library and pinned MSVC SQLite
DLL. Eight native controls cover both DDL kinds in autocommit, deferred,
named and prior-write scopes. Every no-op is `sqlite3_stmt_readonly()==0`;
EXPLAIN contains one read `Transaction` and one journal-mode query.
Deferred/named execution retains a native READ transaction, while Modern
retains RESERVED. Six Modern reserved-lock failure controls return Busy even
though the no-op requires no such lock. Exact change counters remain one,
rowids remain one or two, and no database writes or syncs occur.
These observations establish access/classification, not timing parity or
new multi-connection support.

Modern already owns:

- immutable `ProgramStatementKind`, `ProgramTransactionAccess`, snapshot,
  rollback, mutation-result and write-effect metadata;
- the canonical bytecode verifier, including write-capability checks on
  persistent instructions;
- one coordinator `TransactionStatementOptions` and active statement;
- separate read finishing, actual writer capability and full-rollback paths;
- the shared ADR-0188 VM guard, cleanup and reprepare driver.

All currently admitted SELECT programs are read-only; all admitted
INSERT/UPDATE/DELETE/CREATE/ANALYZE programs are not. Derive the program's
read-only observation from that existing verified statement kind. Do not
add a redundant program flag, schema-noop opcode or metadata registry.

## Decision

### 1. Separate transaction access from statement classification

Expose `BytecodeProgram::read_only()` as the canonical observation of the
existing statement kind. Extend coordinator options with `read_only`,
default-initialized from the preceding `access` field. Existing aggregate
read/write callers retain their current behavior without call-site shims.
Explicit read access with `read_only=false` represents the required no-op
DDL class; reject write access with `read_only=true` before token admission.

Use this classification only where native error policy requires it:
interrupted non-read-only admission or execution fully rolls back; an
interrupted ordinary reader does not. Actual read access still controls
lock admission, writer capability, anonymous journal and read finishing.
Preserve NOMEM/FULL/I/O, ordinary ABORT, explicit cancellation, poisoned
state, pending cleanup and cursor-trip policies.

The bytecode verifier admits read-access CREATE TABLE/INDEX only with an
explicit required snapshot, statement/default-ABORT rollback, no write
effects, no write cursors and their existing nonpublishing mutation result.
All other mutation access restrictions remain. Existing opcode validation
continues to reject persistent writes without write access. A read-access
DDL program must not become source-free or be authorized by an instruction
count heuristic.

### 2. Publish and consume native no-op metadata

Lower only bound no-op CREATE TABLE/INDEX to read transaction access and
explicitly require a database snapshot. Keep their existing statement kind,
single Halt body, zero registers/cursors/effects, default-ABORT rollback,
schema requirement and nonpublishing result metadata.
Ordinary creation retains write access and all current mutation code.

The writable facade passes `program.read_only()` with the current program's
actual access/rollback/journal metadata. The VM receives a database read
context for a no-op and still validates its real schema snapshot; it does
not need a writer. Reprepare uses the replacement's own metadata: a stale
no-op can become a real write, and a former real creation can become a
read-admitted no-op.

Reuse coordinator read finishing and every pending-completion barrier.
No-op execution must not acquire a RESERVED lock, allocate a writer or
anonymous journal, mutate the database, change counters, or finish another
statement's pending commit. Resource failure from an actual database reader
still has native full-rollback behavior; it is not preparation OOM.
Retain rejected binding details behind IF NOT EXISTS and old guard
expiration, Reset, typed cleanup and metadata publication.

This is an admitted-mechanism correction, not admission of read-only public
connections, concurrent statement writers, PRAGMA, WAL or new SQL.

## Two ordered sources and verification

1. Canonical classification and verified capability: first specify the new
   bytecode/coordinator contract in their existing unit owners. Require a
   genuine capability red, then focused success and malformed metadata,
   ordinary-reader interruption versus non-read-only read interruption,
   preserved exact prior writes/named scopes, no writer capability and
   pending cleanup. Complete fast units and independent source review precede
   a normal source commit.
2. No-op producer/facade integration: first reproduce the actual reserved
   lock/Busy failure in existing lowering/session owners. Then publish the
   metadata and pass the same classification. Cover both DDL kinds,
   autocommit/deferred/named/prior-write scopes, valid/malformed snapshots,
   actual read I/O/resource failure, stale no-op-to-write and write-to-no-op,
   Reset/retry, exact contents/counters and pending completion.

Each source requires accepted design, focused red/green, complete fast units,
applicable OOM/ownership/native/sanitizer evidence, unchanged strict quality,
independent SourceAccepted and append-only ledger before its successor.
Retain the original lowered guard failure helpers, adjusting only their
actual capability selection to the generated metadata.

Close the final exact-head change-aware compiler/native/sanitizer/model/
quality and bounded-campaign union, integrated review, normal squash, fresh
actual-main checks and delivered-reference retirement. Do not change
assertions, deadlines, allocation ceilings, stacks or safety policies.
Continue the remaining prerequisite closure immediately after delivery;
an intermediate PR is not the autonomous task's completion boundary.

## GNU Release qualification correction

Source1's hosted GNU13 Release build reports `-Werror=null-dereference`
inside `std::vector::insert` while constructing the negative read-DDL
instruction fixture. The same source and unchanged hosted `-O3` warning
flags reproduce that diagnostic locally. This is a compilation failure,
not evidence of an executed null dereference.

A test-only follow-up uses the existing explicit instruction-list pattern
to construct the identical schema-cookie mutation followed by Halt. It
changes no production behavior, assertions, compiler flags or limits.
The failed prefix and superseded cancelled run remain recorded separately;
neither is successful final-head evidence. Require matching GNU Release
compile success, focused owning cases, complete fast units and requalified
dependent-source closing gates before delivery.
