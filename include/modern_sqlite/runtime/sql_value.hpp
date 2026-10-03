#ifndef MODERN_SQLITE_RUNTIME_SQL_VALUE_HPP_
#define MODERN_SQLITE_RUNTIME_SQL_VALUE_HPP_

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

enum class SqlValueType : std::uint8_t {
  kNull = 0,
  kInteger = 1,
  kReal = 2,
  kText = 3,
  kBlob = 4,
};

enum class TypeAffinity : std::uint8_t {
  kNone,
  kBlob,
  kText,
  kNumeric,
  kInteger,
  kReal,
};

enum class CastTarget : std::uint8_t {
  kInteger,
  kReal,
  kNumeric,
  kText,
  kBlob,
};

enum class SqlComparison : std::uint8_t {
  kEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kIs,
  kIsNot,
};

enum class SqlTruthValue : std::uint8_t {
  kFalse,
  kTrue,
  kNull,
};

class SqlValue {
 public:
  SqlValue() noexcept = default;

  SqlValue(const SqlValue&) = delete;
  SqlValue& operator=(const SqlValue&) = delete;
  SqlValue(SqlValue&&) noexcept = default;
  SqlValue& operator=(SqlValue&&) noexcept = default;
  ~SqlValue() = default;

  [[nodiscard]] static SqlValue Integer(std::int64_t value) noexcept;
  [[nodiscard]] static SqlValue Real(double value) noexcept;
  [[nodiscard]] static SqlValue Text(std::string value);
  [[nodiscard]] static SqlValue Blob(ByteBuffer value) noexcept;

  [[nodiscard]] SqlValue Clone() const;

  [[nodiscard]] SqlValueType type() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> integer_value() const noexcept;
  [[nodiscard]] std::optional<double> real_value() const noexcept;
  [[nodiscard]] std::optional<Utf8View> text_value() const noexcept;
  [[nodiscard]] std::optional<ByteView> blob_value() const noexcept;

 private:
  friend class SqlValueAccess;

  using Storage = std::variant<std::monostate, std::int64_t, double, std::string, ByteBuffer>;

  Storage storage_;
};

[[nodiscard]] SqlValue ApplyAffinity(SqlValue value, TypeAffinity affinity);
[[nodiscard]] SqlValue CastValue(SqlValue value, CastTarget target);

[[nodiscard]] std::strong_ordering CompareSqlValues(const SqlValue& left,
                                                    const SqlValue& right) noexcept;
[[nodiscard]] SqlTruthValue EvaluateSqlComparison(const SqlValue& left, const SqlValue& right,
                                                  SqlComparison comparison) noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_SQL_VALUE_HPP_
