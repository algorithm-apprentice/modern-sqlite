# ADR-0003: Layered Dependency Architecture

- Status: Accepted
- Date: 2026-10-03

## Context

SQLite's intended pipeline is layered, but its implementation contains logical
cycles:

- Parser actions invoke schema and code-generation behavior.
- The planner emits VDBE instructions directly.
- B-tree comparison depends on VDBE record types.
- Schema loading re-enters SQL compilation.
- Pager, cache, and WAL share concrete mutable page structures.

A direct translation would preserve those cycles and prevent a meaningful
topological implementation order.

## Decision

Modern SQLite will use the acyclic architecture in `docs/architecture.md` and
the implementation order in `docs/dependency-dag.md`.

Key cycle-breaking decisions are:

- Extract SQL values, collations, and record comparison below both B-tree and
  VM.
- Make parsing pure and side-effect free.
- Separate catalog model, catalog loading, and binding.
- Make the optimizer produce a physical plan.
- Restrict bytecode emission to the lowering module.
- Give journal backends narrow immutable page views.
- Pass operation contexts and services instead of top-level session objects.
- Prohibit a master internal header.

## Consequences

- Modules can be tested before higher-level orchestration exists.
- Some SQLite structures must be split into syntax, catalog, plan, runtime,
  and storage representations.
- Convenience dependencies that point upward must be replaced with narrower
  contracts.
- The DAG is a build and review invariant, not only documentation.
