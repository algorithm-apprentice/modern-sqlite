# ADR-0011: Typed Errors and Results

- Status: Accepted
- Date: 2026-10-03

## Context

Database failures are expected control-flow outcomes. Integer-only errors lose
type safety and diagnostic context, while exceptions obscure ordinary failure
paths and transaction-state decisions.

Modern SQLite must eventually expose SQLite-compatible primary and extended
result codes without making lower modules include the SQLite C API.

## Decision

- `ErrorCode` is a scoped enum whose numeric values match SQLite primary error
  codes 1 through 26.
- SQLite success, row, done, notice, and warning codes are not errors and
  cannot be converted into `Error`.
- `Error::FromSqliteCode` validates an integer result code, derives its typed
  primary category, and preserves the exact extended code.
- Invalid conversion returns a separate typed `SqliteCodeMappingError`; it
  never returns an empty or success-shaped `Error`.
- `Error::Create` constructs an internal error with its primary SQLite code.
- Every `Error` owns its diagnostic message and captures the factory call site
  with `std::source_location`.
- `Result<T>` is an alias of `std::expected<T, Error>`, and `Status` is
  `Result<void>`.
- Propagation remains explicit through `std::unexpected`. No macro, exception,
  implicit default value, or logging side effect is introduced.

## Consequences

- Expected failures are visible in function signatures.
- Extended SQLite codes can cross internal layers without depending on C
  headers.
- Error messages and source locations support diagnostics but are not part of
  the SQL compatibility contract.
- Ordinary success has no allocated status object.
- Later API layers can map directly to SQLite integers while lower layers keep
  typed categories.
