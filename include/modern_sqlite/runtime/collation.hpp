#ifndef MODERN_SQLITE_RUNTIME_COLLATION_HPP_
#define MODERN_SQLITE_RUNTIME_COLLATION_HPP_

#include <compare>
#include <string_view>

#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

// Defines one immutable text-ordering contract. Distinct byte strings may be
// equivalent, and custom implementations must preserve a deterministic weak
// ordering for their entire lifetime.
class Collation {
 public:
  Collation() = default;
  Collation(const Collation&) = delete;
  Collation& operator=(const Collation&) = delete;
  Collation(Collation&&) = delete;
  Collation& operator=(Collation&&) = delete;
  virtual ~Collation() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::weak_ordering Compare(Utf8View left,
                                                   Utf8View right) const noexcept = 0;
};

[[nodiscard]] const Collation& BinaryCollation() noexcept;
[[nodiscard]] const Collation& NoCaseCollation() noexcept;
[[nodiscard]] const Collation& RTrimCollation() noexcept;

[[nodiscard]] std::weak_ordering CompareSqlValues(const SqlValue& left, const SqlValue& right,
                                                  const Collation& collation) noexcept;
[[nodiscard]] SqlTruthValue EvaluateSqlComparison(const SqlValue& left, const SqlValue& right,
                                                  SqlComparison comparison,
                                                  const Collation& collation) noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_COLLATION_HPP_
