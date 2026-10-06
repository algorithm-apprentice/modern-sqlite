# ADR-0046: Canonical Engineering and Reviewable Delivery

- Status: Accepted
- Date: 2026-10-07

## Context

The project already defines shared byte primitives, typed errors, RAII
ownership, layered dependencies, TDD, and sequential DAG execution. Those
rules are weakened when a local implementation recreates an existing
mechanism with a different container or helper.

Node 36 exposed two workflow failures:

- raw byte ownership in new B-tree tests used `std::vector<std::byte>` instead
  of the existing `ByteBuffer` abstraction; and
- one large node accumulated too much independently reviewable behavior in a
  single pull request.

Both failures increase review cost and make cross-compiler problems easier to
miss. Autonomous development requires stronger consistency and smaller
review boundaries, not fewer gates.

## Decision

### 1. Perform a canonical-pattern audit before design

Before designing a production slice, inspect the repository for existing
implementations of the same semantics:

- ownership and borrowing;
- byte storage and views;
- errors and failure latching;
- naming and state transitions;
- allocation and cleanup;
- test fixtures and failure injection; and
- reference-source mappings.

The design names the canonical mechanism it will reuse. A second equivalent
mechanism is forbidden unless an accepted ADR explains why the canonical
mechanism is insufficient.

### 2. Use one byte-ownership vocabulary

- `ByteBuffer` owns raw bytes.
- `ByteView` borrows immutable bytes.
- `MutableByteView` borrows mutable bytes.
- Typed dynamic collections use `std::vector<T>`.

Direct `std::vector<std::byte>` is permitted only when an API specifically
requires in-place resizing. Each such declaration is named as scratch and
marked with `BYTE_VECTOR_RESIZABLE_SCRATCH`.

### 3. Enforce strict standard C++23

The build sets `CMAKE_CXX_EXTENSIONS` to `OFF`. Production code, tests, and
tools must compile as standard C++23 without GNU language extensions,
compiler-specific semantics, or undefined behavior.

GCC and Clang Debug and Release builds remain required because warnings,
optimization diagnostics, and standard-library implementations are not
identical even for standard-conforming code.

### 4. Split oversized nodes into ordered stacked pull requests

Only one DAG node is active at a time. A node that is too large for reliable
review is divided into ordered stacked PRs:

1. each PR contains one coherent slice;
2. each PR has its own design, TDD, review, and validation evidence;
3. later slices depend only on accepted earlier slices of the same node; and
4. no higher DAG node begins until every slice is complete.

Historical branches and superseded PRs remain traceable but are not merged.

### 5. Continue autonomously without weakening gates

Agents do not wait for routine user replies between approved slices. A clean
PR may be merged autonomously only after required reviews and CI checks pass.
Any failure blocks merging and new feature work until its root cause is fixed.

## Enforcement

- `AGENTS.md` carries the canonical-pattern and byte-ownership rules.
- CI runs `tools/check_engineering_discipline.py`.
- CMake emits strict `-std=c++23` or the compiler-equivalent flag.
- Pull requests run clang-tidy for changed translation units. Cross-compiler
  Debug and Release builds cover header and build-configuration changes.
- `main` always runs the full clang-tidy target. A pull request may request the
  same full target with the `full-quality` label.
- Pull requests require GCC Release, macOS Debug and Release, sanitizers, and
  the Ubuntu performance contract. GCC Debug and the macOS performance
  contract run on `main`.
- CI cancels superseded runs for the same pull request. Benchmark jobs build
  only the binaries required by their selected contract tests.
- Compatible compiler/configuration pairs use isolated ccache namespaces.
  Debug, Release, sanitizer, and operating-system caches are never mixed.
- Review checks pattern reuse in addition to local functional correctness.
- Project graph validation and all existing correctness gates remain required.

## Consequences

### Positive

- Equivalent behavior converges on one implementation.
- Ownership is visible in types.
- Cross-compiler failures are caught before merge.
- Pull requests remain bounded and reviewable.
- Autonomous progress preserves traceability and quality.

### Negative

- Design begins with a repository-wide prior-art check.
- Existing noncanonical patterns may require incremental cleanup.
- Stacked PR maintenance adds dependency bookkeeping.

## References

- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0006: AI-Native Sequential Workflow
- ADR-0009: Build and Toolchain Baseline
- ADR-0010: Byte and Buffer Primitives
- ADR-0045: Existing-Module SQLite Reference Alignment
