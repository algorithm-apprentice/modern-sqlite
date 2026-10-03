# Modern SQLite

Modern SQLite is a ground-up C++23 reimplementation of SQLite. The project
preserves SQLite's durable file-format and observable database semantics while
rebuilding the implementation around explicit ownership, testable durability
boundaries, strict dependency direction, and an AI-native development model.

The project is currently in build-system bootstrap. It contains no production
database code and must not be used with production data.

## Reference snapshot

The initial reference is SQLite 3.54.0:

- Fossil check-in:
  `65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`
- Git mirror commit:
  `cb547ab3e931c7766e24834af5ef6c4578863e3d`
- Check-in date: 2026-10-02

The reference implementation is a behavioral specification, persistent-format
reference, differential-testing oracle, and performance baseline. Modern
SQLite will not be a line-by-line translation and will not preserve accidental
source-level coupling.

## Initial goals

- Read and write the SQLite 3 database file format.
- Preserve atomicity, consistency, recovery, and locking semantics
  incrementally.
- Provide a modern C++23 API with explicit ownership and typed errors.
- Reach observable SQL compatibility through staged, test-driven milestones.
- Keep the production dependency graph acyclic and independently testable.
- Treat performance parity with pinned SQLite as an engineering constraint,
  not a late optimization phase.
- Make the repository directly usable by AI agents through explicit contracts,
  machine-readable task dependencies, executable specifications, and
  traceability.

## Initial non-goals

- Immediate support for the complete SQLite SQL language or C API.
- Compatibility with SQLite's internal VDBE opcode set.
- WAL mode, shared-cache mode, extensions, virtual tables, or attached
  databases in the first writable milestone.
- A migration framework before an incompatible deployed format exists.
- Distributed operation, replication, or a client-server protocol.

## Architecture

- [SQLite source analysis and target architecture](docs/architecture.md)
- [Implementation dependency DAG](docs/dependency-dag.md)
- [Architecture decision records](docs/adr/README.md)
- [AI project manifest](project/manifest.json)
- [Machine-readable module graph](project/module-graph.json)

## Build

The supported local bootstrap requires CMake 3.25 or newer, Ninja, Python 3.10
or newer, and a C++23 compiler:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

Validate the project graph or query the single active/next-ready node with:

```sh
python3 tools/project_graph.py validate
python3 tools/project_graph.py next-ready --json
```

The architecture ADRs are accepted. Implementation proceeds one reviewed DAG
node at a time.
