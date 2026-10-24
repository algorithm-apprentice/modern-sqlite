# ADR-0149: Matched Parser and Preparation Evidence

- Status: Accepted
- Date: 2026-10-24
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 14

## Completed mechanism research

PR #254 closes the admitted function mechanism. The parser alternative and
binder preparation remain unresolved; this ADR adds their first bounded
qualification input, not another feature roadmap or a decision that the
alternative already meets its requirements.

Modern `ParseOne` is pure recursive descent/Pratt construction of the
source-preserving immutable arena. It includes token adaptation, explicit
construction/recursion bounds, statement-source copying and AST validation.
The returned owner is destroyed nonrecursively. `ReadSession::Prepare`
additionally performs schema refresh, binding, logical/physical planning,
lowering, verification, VM construction and statement metadata ownership.

SQLite's public prepare API is not a parse-only oracle. The pinned source
maps the native boundary as follows:

| Responsibility | Pinned source |
|---|---|
| Tokens, contextual keywords, synthetic semicolon, tail and parser teardown | `tokenize.c:600-765` |
| Native Lemon stack, checked growth and initialization | `parse.c:2420-2511` |
| Root SELECT's schema/code-generation action | `parse.y:520-528`, generated `parse.c:4271-4281` |
| Final bytecode generation after a complete command | generated `parse.c:3841-3844`; `build.c:142-280` |
| Parser context initialization, cleanup and outer-owner restoration | `prepare.c:592-694` |
| Public prepare, schema/compiler completion and statement ownership | `prepare.c:709-1000` |

The special rename/unmap modes do not isolate ordinary SELECT syntax:
the root action still calls schema reading and `sqlite3Select`, whose entry
creates a VDBE and continues into semantic preparation.

A disposable main-agent control verifies a minimal native syntax adapter:
exactly two guarded semantic callbacks in a generated copy of the pinned
amalgamation. With syntax mode enabled, a nonexistent column/table SELECT
parses successfully without a VDBE, malformed input still fails, and the
first-semicolon tail is preserved. Normal mode still reports the ordinary
prepare error. The original pinned source is unchanged. This is a research
control, not yet qualified native timing or a behavior oracle.

## Decision

### 1. Keep pure syntax and complete preparation distinct

Add one bounded benchmark family with explicit `parse` and `prepare` phases.
The first matched set covers common SELECT syntax: short parameterized SQL,
wide lists at two fixed widths, nested expressions at two fixed depths, and
short/wide/deep malformed controls. Freeze exact SQL and counts before timing.
All cases stay within both engines' common admitted surface and limits.

`parse` includes tokenization, native/Modern AST construction, validation or
grammar checks, error/tail publication and complete AST/parser-context cleanup.
It does not include catalog resolution, planning, bytecode or VM construction.
`prepare` uses the unchanged original SQLite public API and Modern public
session facade on the same existing read fixture, with warmed catalogs and
identical read configuration. It includes statement metadata and finalization
without stepping or mutating the database.

Do not compare pure Modern parsing with complete SQLite preparation or
relabel an authorization failure, tokenizer-only pass or sampled time as
parse-only work. Preparation errors are not interchangeable with parser
errors. Statement families outside the native syntax adapter's SELECT scope
are not silently counted as matched parse-only coverage.

### 2. Isolate the benchmark-only native syntax adapter

Generate a copy from the exact pinned `sqlite3.c`, verifying its manifest
identity and SHA-256 before applying two cardinality-checked substitutions:

- skip only the root SELECT schema/`sqlite3Select` action in syntax mode;
  retain its ordinary `sqlite3SelectDelete`; and
- replace only the root `sqlite3FinishCoding` call with native `SQLITE_DONE`
  publication in that mode.

Lemon tables, shift/reduce/growth algorithms, tokenizer, AST builders,
destructors, expression checks and error handling remain unchanged.
Keep the mode fixed for the synchronous call. Reject unsupported adapter
statement kinds rather than executing other semantic actions accidentally.
Preserve the original reference files and original SQLite preparation target.

Compile the adapter's SQLite APIs with translation-unit-local linkage.
Expose only a narrow benchmark C bridge with explicit create/parse/destroy
ownership, so it cannot replace or interpose the unchanged public SQLite
implementation linked for `prepare`. C++ owns the bridge handle through RAII.
The amalgamation is included before bridge/system headers so its native
platform feature macros take effect before libc declarations; the bridge
explicitly includes the standard limits it uses.
The adapter is not linked into the ordinary Modern library, oracle, fuzz
engine or existing benchmark families.

Raw identity records distinguish original native prepare from derived native
syntax measurements, recording original/generated hashes, both exact
substitutions, compiler/options, mode and cleanup counters. Never claim the
derived copy is an unmodified SQLite behavior/performance oracle.

### 3. Reuse the current measurement and validation mechanisms

Reuse existing benchmark clock, digest, JSON/identity and allocation helpers,
and Python bounded-child/provenance/schedule/ratio infrastructure where their
semantics apply. Add no production executor, timer framework, callback plugin
system, general benchmark registry or duplicated SQL-value parser.

The phase runner owns phase-appropriate work and lifecycle accounting:
each iteration must actually parse or prepare/finalize once. Do not inherit
the existing execute-only prepare/reset counts. Retain equal engine work,
warmup, nine ordinary samples in the existing alternating three-round order,
wall and process CPU clocks, allocation/fixed-work diagnostics and bounded
child deadlines. Timing and diagnostics are separate binaries.
Smoke runs replay 64 real operations so the observed one-microsecond GNU
steady-clock tick cannot collapse a single fast syntax call to zero.
Both engines report that same bounded work; elapsed values are never clamped.

Report accepted/rejected inputs, consumed offsets, result/parameter metadata
where phase-comparable, actual operations and deterministic work digest.
Native and Modern AST node/stack counts are separate representation metrics,
not forced equal. Record native lookaside and Modern owned-arena costs
truthfully rather than disabling one engine's proven allocation mechanism
to improve a ratio.

Controls must prove syntax/semantic separation, no VDBE or catalog mutation
in native syntax mode, source-file immutability, lifecycle cleanup after
success/error and same-context retry, malformed-transform rejection and
strict current build/input identity. Private old-machine files are not
inputs: committed source, fixed reference identity and generation commands
must recreate the family on another machine.

### 4. Treat the measurements as evidence, not automatic closure

The initial family establishes the previously missing matched SELECT
parse/prepare baseline. It does not by itself close every parser statement
family or the binder row, approve a slower algorithm without justification,
change ADR-0028's resource contract, or weaken any existing compatibility
boundary.

After source-qualified measurements, attribute short/wide/deep/error work
and retain or repair the parser with an explicit supported disposition under
ADR-0111. Additional required current statement coverage is added only when
that disposition needs it. Binder ownership/replay remains the next ordered
resolution; later SQL features remain blocked.

## Sequential qualification and delivery

Review this design before repository implementation. Develop generator,
bridge and CLI behavior through focused failing tests, including wrong
source/hash/anchor cardinality, unsupported syntax-mode statements, semantic
error controls and real cleanup. Use existing fixtures and helpers rather
than adding databases or a compatibility corpus.

Keep each committed source buildable, owning tests and complete fast units
green, and review before commit. Run the actual change-aware closing plan,
native controls, fresh matched inputs and required hosted policy without
claiming unselected work. Publish the complete source/evidence chain before
final integrated acceptance, guarded squash and named cleanup.

Production parser, catalog, binder, planner, VM and API behavior are unchanged
by this evidence source. No later DAG node or SQL surface is delivered.
