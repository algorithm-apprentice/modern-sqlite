# ADR-0014: Text and UTF-8 Primitives

- Status: Accepted
- Date: 2026-10-03

## Context

SQL source, identifiers, diagnostics, text values, and later scalar functions
need a shared byte-oriented text vocabulary. SQLite preserves malformed UTF-8
bytes when no encoding conversion occurs, classifies tokens by byte, and uses
locale-independent ASCII case conversion. Treating every text view as already
valid Unicode would reject inputs that SQLite accepts, while unchecked pointer
arithmetic would make source diagnostics and malformed-input handling unsafe.

The pinned SQLite 3.54.0 behavior is defined by `src/utf.c` for UTF-8
decoding, `src/global.c` for lexical character classes and ASCII case maps,
and `src/tokenize.c` for identifier-byte rules.

## Decision

- `Utf8View` is a non-owning, byte-preserving wrapper around
  `std::string_view`. Construction does not validate, normalize, terminate, or
  copy the bytes, so malformed UTF-8 and embedded NUL bytes remain
  representable.
- `SourceSpan` is a checked half-open byte range. It uses the base
  `ByteOffset` and `ByteCount` vocabulary, rejects reversed or overflowing
  construction, and validates bounds before producing a subview.
- Strict UTF-8 decoding and validation are opt-in. They accept Unicode scalar
  values through U+10FFFF and report the first malformed byte or truncation
  offset with a structured, allocation-free error.
- Strict validation never repairs or discards input. Callers that require
  SQLite's permissive byte preservation continue to use the original view.
- SQLite lexical helpers are byte-oriented and locale-independent. They match
  SQLite's whitespace, alphabetic, digit, hexadecimal, quote, and identifier
  classes for all 256 byte values.
- Identifier bytes are ASCII letters, digits, `_`, `$`, and every byte with
  the high bit set. This includes bytes that are not independently valid
  UTF-8, matching SQLite tokenization.
- Case conversion is ASCII-only. Bytes outside `A` through `Z` or `a` through
  `z` are unchanged; Unicode case folding is intentionally not introduced.
- Views, spans, decoding, validation, classification, and case conversion do
  not allocate. Character classification uses a compile-time table and UTF-8
  decoding has an ASCII fast path.

## Consequences

- The lexer and diagnostics can retain exact byte offsets into borrowed SQL
  text without pointer ownership or locale dependence.
- Later SQL-value and function modules can preserve arbitrary SQLite text
  bytes while choosing strict validation only at boundaries that require it.
- Valid UTF-8 iteration is safe and deterministic, and malformed input reports
  the precise failure offset instead of reading beyond the supplied view.
- Full Unicode normalization and case folding remain outside the SQLite
  compatibility contract and require a separate decision if ever needed.
