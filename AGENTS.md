# Agent Contract

This repository is designed for AI-assisted development. Every agent must
follow the same architecture, testing, review, and traceability rules.

## Required context

Read these files before changing production code:

1. `project/manifest.json`
2. `docs/architecture.md`
3. `docs/dependency-dag.md`
4. Applicable files under `docs/adr/`
5. The selected node in `project/module-graph.json`

Do not begin a node whose prerequisites are incomplete.

## Language

Use English for code, identifiers, comments, tests, documentation, ADRs,
benchmark reports, pull-request text, and commit messages.

## Development workflow

- Use documentation-first development. Architectural or behavioral decisions
  require an ADR before implementation.
- Develop exactly one coherent DAG node at a time.
- Use one pull request per node and wait for review before beginning the next
  node.
- Review design documents and code changes before committing them.
- Keep every change linked to a module-graph node and relevant ADRs.
- Do not add speculative abstractions, migration layers, compatibility shims,
  or plugin mechanisms without a current accepted requirement.

## Test-driven development

New or changed production behavior follows red-green-refactor:

1. Add one focused test that specifies observable behavior.
2. Run it and record the expected failure.
3. Implement the smallest correct change.
4. Run the focused test and the complete fast unit tier.
5. Refactor only while the tests remain green.

Behavior-preserving refactors start from a green baseline and do not require an
artificial failing test. Documentation-only changes are reviewed for
correctness, consistency, references, and graph ordering.

Tests belong to the module that introduces the behavior. Do not defer unit,
format, malformed-input, failure-injection, or ownership tests to a later
"testing phase".

## Architecture rules

- Dependencies point only downward in `docs/architecture.md`.
- Lower layers must never include the public API, session, planner, VM, or AST
  merely for convenience.
- Persistent-format modules perform no filesystem I/O.
- The parser produces syntax data and performs no catalog mutation or bytecode
  generation.
- The B-tree depends on shared value, collation, and record contracts, never on
  the VM.
- The planner produces a physical plan; only the lowering module emits
  bytecode.
- The VM executes bytecode and does not depend on parser or planner types.
- Expected database errors use typed results, not exceptions or silent
  fallbacks.
- Owning raw pointers are forbidden. Borrowing, pinning, and lifetime rules
  must be explicit.
- Every persistent state transition documents its ordering and has a
  deterministic fault-injection test.

## Compatibility and performance

- Use the pinned SQLite snapshot as the format, behavior, and performance
  oracle.
- Add independent golden vectors for persistent encoders and decoders.
- Verify both interoperability directions: SQLite writes/Modern reads and
  Modern writes/SQLite reads.
- Match benchmark configuration before comparing engines: page size, cache
  size, journal mode, synchronous mode, mmap, temp storage, schema, corpus,
  and SQL.
- Establish a baseline before optimizing a performance-sensitive node.
- Optimize only after a repeatable bottleneck is supported by matched
  benchmarks, fixed-work diagnostics, or profiling.
- Keep benchmark instrumentation out of the ordinary production library.
- Never trade away corruption detection, transaction correctness, or explicit
  errors for an unmeasured speed claim.

## Completion

A production node is complete only when:

- Its prerequisites are complete.
- Its behavior was developed test-first.
- Focused tests cover normal, boundary, malformed, and failure behavior where
  applicable.
- Persistent behavior has golden, interoperability, or crash evidence as
  applicable.
- Performance-sensitive behavior has a recorded baseline.
- Public contracts and ADRs are current.
- The dependency graph remains acyclic.
- Design and implementation review found no unresolved issue.
