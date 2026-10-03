#include "modern_sqlite/runtime/collation.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>

namespace modern_sqlite {
namespace {

[[nodiscard]] std::uint8_t ByteAt(std::string_view value, std::size_t offset) noexcept {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(value[offset]));
}

[[nodiscard]] std::weak_ordering CompareLengths(std::size_t left, std::size_t right) noexcept {
  if (left < right) {
    return std::weak_ordering::less;
  }
  if (left > right) {
    return std::weak_ordering::greater;
  }
  return std::weak_ordering::equivalent;
}

[[nodiscard]] std::weak_ordering CompareBinary(Utf8View left, Utf8View right) noexcept {
  const std::string_view left_bytes = left.bytes();
  const std::string_view right_bytes = right.bytes();
  const std::size_t common_size = std::min(left_bytes.size(), right_bytes.size());
  if (common_size != 0) {
    const int comparison = std::memcmp(left_bytes.data(), right_bytes.data(), common_size);
    if (comparison < 0) {
      return std::weak_ordering::less;
    }
    if (comparison > 0) {
      return std::weak_ordering::greater;
    }
  }
  return CompareLengths(left_bytes.size(), right_bytes.size());
}

class BinaryCollationImplementation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "BINARY"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    return CompareBinary(left, right);
  }
};

class NoCaseCollationImplementation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "NOCASE"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    const std::string_view left_bytes = left.bytes();
    const std::string_view right_bytes = right.bytes();
    const std::size_t common_size = std::min(left_bytes.size(), right_bytes.size());

    std::size_t offset = 0;
    while (offset < common_size) {
      const std::uint8_t left_byte = ByteAt(left_bytes, offset);
      const std::uint8_t right_byte = ByteAt(right_bytes, offset);
      if (left_byte == 0U || SqliteToLower(left_byte) != SqliteToLower(right_byte)) {
        break;
      }
      ++offset;
    }

    if (offset == common_size) {
      return CompareLengths(left_bytes.size(), right_bytes.size());
    }

    const std::uint8_t left_folded = SqliteToLower(ByteAt(left_bytes, offset));
    const std::uint8_t right_folded = SqliteToLower(ByteAt(right_bytes, offset));
    if (left_folded < right_folded) {
      return std::weak_ordering::less;
    }
    if (left_folded > right_folded) {
      return std::weak_ordering::greater;
    }
    return CompareLengths(left_bytes.size(), right_bytes.size());
  }
};

class RTrimCollationImplementation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "RTRIM"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    std::string_view left_bytes = left.bytes();
    std::string_view right_bytes = right.bytes();
    while (!left_bytes.empty() && ByteAt(left_bytes, left_bytes.size() - 1U) == 0x20U) {
      left_bytes.remove_suffix(1);
    }
    while (!right_bytes.empty() && ByteAt(right_bytes, right_bytes.size() - 1U) == 0x20U) {
      right_bytes.remove_suffix(1);
    }
    return CompareBinary(Utf8View{left_bytes}, Utf8View{right_bytes});
  }
};

[[nodiscard]] SqlTruthValue ToTruthValue(bool value) noexcept {
  return value ? SqlTruthValue::kTrue : SqlTruthValue::kFalse;
}

[[nodiscard]] SqlTruthValue EvaluateOrdering(std::weak_ordering ordering,
                                             SqlComparison comparison) noexcept {
  switch (comparison) {
    case SqlComparison::kEqual:
    case SqlComparison::kIs:
      return ToTruthValue(ordering == std::weak_ordering::equivalent);
    case SqlComparison::kNotEqual:
    case SqlComparison::kIsNot:
      return ToTruthValue(ordering != std::weak_ordering::equivalent);
    case SqlComparison::kLess:
      return ToTruthValue(ordering == std::weak_ordering::less);
    case SqlComparison::kLessEqual:
      return ToTruthValue(ordering != std::weak_ordering::greater);
    case SqlComparison::kGreater:
      return ToTruthValue(ordering == std::weak_ordering::greater);
    case SqlComparison::kGreaterEqual:
      return ToTruthValue(ordering != std::weak_ordering::less);
  }
  std::terminate();
}

}  // namespace

const Collation& BinaryCollation() noexcept {
  static const BinaryCollationImplementation collation;
  return collation;
}

const Collation& NoCaseCollation() noexcept {
  static const NoCaseCollationImplementation collation;
  return collation;
}

const Collation& RTrimCollation() noexcept {
  static const RTrimCollationImplementation collation;
  return collation;
}

std::weak_ordering CompareSqlValues(const SqlValue& left, const SqlValue& right,
                                    const Collation& collation) noexcept {
  if (left.type() == SqlValueType::kText && right.type() == SqlValueType::kText) {
    const auto left_text = left.text_value();
    const auto right_text = right.text_value();
    if (!left_text.has_value() || !right_text.has_value()) {
      std::terminate();
    }
    return collation.Compare(*left_text, *right_text);
  }
  return CompareSqlValues(left, right);
}

SqlTruthValue EvaluateSqlComparison(const SqlValue& left, const SqlValue& right,
                                    SqlComparison comparison, const Collation& collation) noexcept {
  if (left.type() == SqlValueType::kText && right.type() == SqlValueType::kText) {
    return EvaluateOrdering(CompareSqlValues(left, right, collation), comparison);
  }
  return EvaluateSqlComparison(left, right, comparison);
}

}  // namespace modern_sqlite
