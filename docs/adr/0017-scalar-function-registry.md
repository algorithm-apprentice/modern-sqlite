# ADR-0017: Scalar Function Registry

- Status: Accepted
- Date: 2026-10-03

## Context

The binder and virtual machine need one shared contract for resolving and
invoking scalar SQL functions. SQLite 3.54.0 resolves function names
case-insensitively for ASCII bytes, prefers an exact arity over a variadic
definition, records whether a function is deterministic, and gives
collation-sensitive functions the collation selected for their expression.

`SqlValue` is intentionally move-only. Function arguments therefore need a
borrowed representation that does not copy TEXT or BLOB payloads, while a
successful invocation must return an independently owned value. Function
errors must use the typed `Result` contract rather than a NULL-shaped
fallback.

Several SQLite spellings that appear in the function table are not ordinary
eager scalar callbacks. `coalesce`, `ifnull`, and `iif` are lowered to virtual
machine control flow so unused arguments are not evaluated. Treating them as
ordinary callbacks would silently change SQL semantics.

## Decision

- `ScalarFunction` is an immutable descriptor containing:
  - a stable borrowed name;
  - an exact or minimum accepted argument count;
  - deterministic or non-deterministic metadata;
  - whether invocation semantics use the selected collation; and
  - a stateless function pointer.
- `FunctionRegistry` borrows an immutable span of descriptors. Descriptor
  storage and name storage must outlive the registry and every resolved
  pointer.
- The process-wide core registry uses static storage. Successful lookup does
  not allocate, and a resolved call dispatches directly through one function
  pointer without repeating name lookup.
- Names compare with SQLite ASCII case folding. Bytes outside ASCII are
  compared unchanged.
- Resolution distinguishes an unknown name from a known name with the wrong
  arity. Exact signatures outrank minimum-arity signatures; otherwise the
  first matching descriptor wins.
- Scalar callbacks receive:
  - a borrowed `std::span<const SqlValue>`; and
  - a narrow invocation context containing the selected `Collation`.
- Callbacks return `Result<SqlValue>`. Returned values own their payloads.
  Functions that return one of their arguments clone it explicitly.
- The initial core registry contains ordinary deterministic scalar functions:
  - `typeof`;
  - `length`;
  - `abs`;
  - `lower`;
  - `upper`;
  - `sign`;
  - `nullif`;
  - scalar `min` with at least two arguments; and
  - scalar `max` with at least two arguments.
- `nullif`, scalar `min`, and scalar `max` use the invocation collation for
  TEXT-to-TEXT comparisons. Other storage-class comparisons retain the
  ordering from ADR-0015.
- Scalar `min` returns the rightmost value among equivalent minima. Scalar
  `max` returns the leftmost value among equivalent maxima, matching SQLite's
  callback behavior.
- `length` preserves SQLite's permissive UTF-8 counting and stops at the first
  embedded NUL. It does not validate or normalize text.
- `abs` reports `integer overflow` for the minimum signed 64-bit integer.
  Invalid textual or BLOB numeric input produces REAL `0.0`, as in SQLite.
- Lazy special forms (`coalesce`, `ifnull`, `iif`, `if`, `likely`,
  `likelihood`, and `unlikely`) are deferred to binder and bytecode lowering.
  Aggregate/window functions, connection-local replacement, stateful
  application callbacks, security flags, subtypes, and session-dependent
  non-deterministic functions remain in their later DAG nodes.

## Consequences

- Binder output can retain a stable pointer to a resolved core function, and
  the VM can invoke it without name hashing or argument copies.
- The registry contract supports deterministic metadata and exact versus
  minimum arity without exposing SQLite's internal negative arity encodings.
- Collation-dependent functions remain independent of binder internals.
- TEXT or BLOB results cloned from arguments allocate by design; numeric and
  NULL results do not.
- Short-circuit SQL behavior cannot accidentally become eager through the
  scalar callback API.
- Future connection-local function registration will require an owning
  overlay and invalidation policy; that work is intentionally not hidden in
  this immutable core registry.
