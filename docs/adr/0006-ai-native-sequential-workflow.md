# ADR-0006: AI-Native Sequential Workflow

- Status: Accepted
- Date: 2026-10-03

## Context

AI agents need explicit context, bounded tasks, machine-readable dependencies,
executable acceptance criteria, repeatable automation, and traceability.
Unstructured prompts and implicit repository knowledge create inconsistent
architecture and unverifiable changes.

The project also requires sequential development and review rather than
parallel implementation of independent features.

## Decision

The repository will provide all six AI-native capabilities:

1. Agent-readable contracts through `AGENTS.md`, architecture documents, and
   ADRs.
2. A machine-readable task graph in `project/module-graph.json`.
3. Tests and compatibility matrices as executable specifications.
4. Context engineering through `project/manifest.json` and bounded module
   context.
5. Repeatable agent automation for graph validation, ready-node selection,
   builds, tests, compatibility, crash, fuzz, and benchmark runs.
6. Traceability from requirements to ADRs, DAG nodes, tests, implementation,
   benchmarks, and pull requests.

Only one DAG node is implemented at a time. Each node is delivered in one
coherent pull request and must be reviewed before the next node begins.

The bootstrap node will add validation that:

- The module graph is acyclic.
- The canonical order is a valid topological sort.
- Referenced ADR and context paths exist.
- No completed node has incomplete prerequisites.
- Test selections cannot pass with zero tests.

## Consequences

- Agents receive bounded execution tasks instead of broad feature prompts.
- Repository state, not conversation history, is the durable source of truth.
- Automation failures are explicit and never success-shaped.
- Updating behavior requires updating its traceability chain.
