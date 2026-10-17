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
6. `project/progress.json`

Do not begin a node whose prerequisites are incomplete.

Use `python3 tools/project_graph.py validate` before committing project metadata
and `python3 tools/project_graph.py next-ready --json` to select work.

## Language

Use English for code, identifiers, comments, tests, documentation, ADRs,
benchmark reports, pull-request text, and commit messages.

## Development workflow

- Use documentation-first development. Architectural or behavioral decisions
  require an ADR before implementation.
- Develop exactly one coherent DAG node at a time.
- Keep each pull request reviewable. An oversized DAG node normally uses one
  coherent slice per ordered pull request. An accepted delivery ADR may batch
  two through four related, buildable slice commits; every commit requires
  accepted design, focused red/green or a green refactor baseline, complete
  fast units, implementation review, ledger evidence, and node/ADR
  traceability before the next slice begins. ADR-0106 alone authorizes the
  seven-commit node-43 prerequisite-closure exception.
- Run integrated closing gates against the final exact batch head. Changing an
  earlier slice invalidates every dependent suffix's evidence and review until
  that suffix is requalified. No later DAG node begins before the active node
  is complete.
- Continue autonomously through reviewed slices without waiting for routine
  user replies. Merge only after all required review and validation gates pass.
- Review design documents and code changes before committing them.
- Keep every change linked to a module-graph node and relevant ADRs.
- Do not add speculative abstractions, migration layers, compatibility shims,
  or plugin mechanisms without a current accepted requirement.

## Internal consistency

- Before designing a slice, inventory existing abstractions, ownership types,
  error handling, naming, state machines, and test helpers that provide the
  same semantics.
- Select one canonical implementation and reuse it everywhere. Equivalent
  behavior must not acquire a second container, helper, or algorithm merely
  for local convenience.
- Owned raw bytes use `ByteBuffer`; borrowed bytes use `ByteView` or
  `MutableByteView`.
- `std::vector<std::byte>` is allowed only for an explicitly documented
  resize-required scratch API. Mark each declaration with
  `BYTE_VECTOR_RESIZABLE_SCRATCH`.
- Use strict standard C++23. Compiler language extensions and undefined
  behavior are forbidden.
- Any necessary exception to a canonical pattern requires an ADR that states
  its scope and why the shared mechanism is insufficient.

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

## Verification efficiency

- Use `python3 tools/verify.py plan --base-ref <base>` to select local closing
  checks from the actual change. A plan is not execution evidence; unknown
  changes escalate and outstanding compiler/hosted requirements stay explicit.
- The focused regression and complete fast unit tier remain mandatory.
  `tools/run_fast_tests.py` is an optional bounded fast-local layer, not a
  substitute for isolated qualification, models, or required hosted gates.
- Run configuration-independent checks once per unchanged-input local plan.
  Use `tools/verify.py run-shared` to reconcile every excluded configuration
  against the owner before reusing its result; missing/different instances fail.
  Do not repeat successful matching work after review-only or documentation
  updates, and do not reuse differently configured or incomplete evidence.
- Use `tools/verify.py build-native` for the canonical native targets rather
  than rebuilding unrelated default-ALL tests. Keep native timings isolated.
- Follow ADR-0102 and `docs/verification.md`; preserve assertions, workloads,
  allocation limits, thresholds, and deadlines.

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
