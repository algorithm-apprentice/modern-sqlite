# ADR-0158: Native Scalar Membership Hash Prerequisite

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: bounded prerequisite for ADR-0053's IN query-RHS admission

## Context and completed source mapping

Start from PR #262's verified green main `8f00719`, tree
`308e4fc16ec500e2557828b982ee8d4ce7ca465b`. The shared expression/query owner,
read producers and reusable membership relation already exist. SELECT/VALUES
membership is not a scalar LIMIT-1 destination.

The pinned SQLite 3.54.0 source chooses stored rowid/index access or a
materialized relation in `expr.c:3297-3518`. Its materialized-query path
creates a statement-lived filter in `expr.c:3660-3830`; `select.c:1438-1461`
and `select.c:3279-3295` add result keys, and `expr.c:4299-4339` uses the
filter only where false and NULL destinations coincide. A real sorter can
disable population/use, and non-BINARY comparison disables the filter.
Executed native SQL/EXPLAIN controls confirm these are distinct paths.

The scalar component of the exact native hash is defined by
`vdbe.c:686-734`, with insertion/probing at `vdbe.c:9139-9196`.
REAL conversion delegates to `sqlite3VdbeIntValue()` /
`sqlite3RealToI64()` in `vdbemem.c:641-655,879-883`. Reset/finalization owns
the filter's value memory; no persistent hash representation is involved.

Forty-eight executed public-counter controls cover twenty-four numeric and
byte inputs. They confirm both the predicted occupied 80,000-bit filter slot
and its adjacent unoccupied slot against the pinned MSVC oracle. These
controls validate native filter positions, not direct exposure of the entire
64-bit hash. Independent full-width vectors cover the component arithmetic.

Modern SQLite has no existing Bloom hash. Replacing it with `std::hash`,
record hashing, a cryptographic digest or a text-normalizing algorithm would
not translate the pinned mechanism. This prerequisite introduces one pure
scalar kernel before its relation/VM integration; it admits no SQL surface.

## Decision

### One SQL-value kernel

Add `HashSqlValueForMembership(const SqlValue&) noexcept` beside the existing
shared SQL-value conversions. It returns an unsigned 64-bit component:

1. NULL contributes zero.
2. INTEGER contributes its value converted modulo 2^64.
3. REAL contributes its native truncated/saturated integer value, using the
   existing `CoerceIntegerForBitwise` numeric conversion. Do not copy its
   conversion algorithm or reinterpret floating-point bits.
4. TEXT contributes its byte length and a native-order unsigned word copied
   from its first at most eight bytes into a zero-initialized word. At eight
   bytes or more, also add the word formed by the last eight bytes. The two
   samples intentionally overlap, including reading an eight-byte text twice.
5. BLOB contributes its complete byte length and its first at most eight bytes
   copied into a zero-initialized native-order word, without a tail sample.

All additions use defined unsigned wraparound. Use standard `std::memcpy`,
not unaligned loads, aliasing casts or a fixed little-endian decoder.
The transient native hash is host-order-dependent; it is not database-file
encoding or a stable public hash protocol. Already materialized zero bytes
in a BLOB participate normally in its complete length and prefix.

This is a non-cryptographic SQL membership primitive, not a security digest.
It deliberately has collisions and no type salt. A future caller must
establish BINARY comparison, apply the native key affinity beforehand and
retain the exact B-tree check after a possible match. In particular, NOCASE
or other collation-equivalent text must not be passed through a filter that
assumes this hash preserves its equality.

### Ownership, boundedness and canonical mechanisms

Observe the existing value by const reference. Do not clone it, apply an
affinity, replace retained scratch, allocate bytes or add another value
representation, hash registry, container or numeric converter.
Byte handling shares one bounded prefix kernel, with the TEXT-only tail
addition. Work is independent of payload length after observing that length:
at most sixteen bytes are copied into scalar local words.

Keep all ordinary numeric, comparison, coercion and ownership behavior
unchanged. The helper is an ordered current requirement of native query
membership, not an independent hash-table or optimization framework.

## Test-first delivery and boundaries

Owning SQL-value tests specify fixed INTEGER/REAL vectors, finite conversion
boundaries, NULL, negative zero, normalized NaN, empty values, embedded NUL,
UTF-8 bytes and the seven/eight/nine/sixteen-byte sampling boundaries.
Byte vectors record independent expected words for native little/big endian,
including overlapping TEXT samples and the different BLOB rule.

Record the focused API-absence build red before implementation; do not report
it as an executed failing runtime case. After implementation, run the focused
vectors, the existing standalone allocation tier and complete fast units.
The allocation test observes prepared long TEXT/BLOB values while allocation
is forbidden, and verifies unchanged type, bytes, address and capacity.
Review the bounded implementation and final exact-tree evidence before
publication.

The bounded design review checks the complete scalar algorithm against its
pinned implementation, numeric conversion, byte ownership, BINARY-only use,
test boundaries and the explicit absence of a broader SQL admission.

This slice does not add a filter object, bytecode, cursor-retention exception,
query construction identities, SELECT/VALUES RHS, row values or IN-driven
WHERE loops. Those remain reviewed owning integrations. Existing native
storage-choice, RHS reuse and constructor-phase controls must constrain
them; this helper does not justify simplifying those mechanisms.

ADR-0053 defers new timing families and optimization until feature parity.
Fixed native mechanism, exact vectors and zero-allocation checks are not a
whole-engine or end-to-end performance-parity claim.
