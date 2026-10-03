#ifndef MODERN_SQLITE_TEXT_TEXT_HPP_
#define MODERN_SQLITE_TEXT_TEXT_HPP_

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"

namespace modern_sqlite {

enum class SourceSpanError {
  kReversedBounds,
  kOffsetOverflow,
  kOutOfRange,
};

class SourceSpan {
 public:
  constexpr SourceSpan() noexcept = default;

  [[nodiscard]] static constexpr std::expected<SourceSpan, SourceSpanError> FromBounds(
      ByteOffset begin, ByteOffset end) noexcept {
    if (end < begin) {
      return std::unexpected(SourceSpanError::kReversedBounds);
    }
    return SourceSpan{begin, end};
  }

  [[nodiscard]] static constexpr std::expected<SourceSpan, SourceSpanError> FromOffsetAndLength(
      ByteOffset offset, ByteCount length) noexcept {
    if (length.value() > std::numeric_limits<std::size_t>::max() - offset.value()) {
      return std::unexpected(SourceSpanError::kOffsetOverflow);
    }
    return SourceSpan{offset, ByteOffset{offset.value() + length.value()}};
  }

  [[nodiscard]] constexpr ByteOffset begin() const noexcept { return begin_; }
  [[nodiscard]] constexpr ByteOffset end() const noexcept { return end_; }
  [[nodiscard]] constexpr ByteCount length() const noexcept {
    return ByteCount{end_.value() - begin_.value()};
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return begin_ == end_; }

  constexpr auto operator<=>(const SourceSpan&) const noexcept = default;

 private:
  constexpr SourceSpan(ByteOffset begin, ByteOffset end) noexcept : begin_(begin), end_(end) {}

  ByteOffset begin_;
  ByteOffset end_;
};

class Utf8View {
 public:
  constexpr Utf8View() noexcept = default;
  constexpr explicit Utf8View(std::string_view bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] constexpr const char* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] constexpr std::size_t size_bytes() const noexcept { return bytes_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] constexpr std::string_view bytes() const noexcept { return bytes_; }

  constexpr auto operator<=>(const Utf8View&) const noexcept = default;

 private:
  std::string_view bytes_;
};

[[nodiscard]] inline std::expected<Utf8View, SourceSpanError> Slice(Utf8View value,
                                                                    SourceSpan span) noexcept {
  if (span.end().value() > value.size_bytes()) {
    return std::unexpected(SourceSpanError::kOutOfRange);
  }
  if (value.empty()) {
    return Utf8View{};
  }
  return Utf8View{std::string_view{value.data() + span.begin().value(), span.length().value()}};
}

enum class Utf8ErrorKind {
  kEndOfInput,
  kOffsetOutOfRange,
  kUnexpectedContinuationByte,
  kInvalidContinuationByte,
  kTruncatedSequence,
  kOverlongEncoding,
  kSurrogateCodePoint,
  kCodePointOutOfRange,
};

struct Utf8Error {
  Utf8ErrorKind kind;
  ByteOffset offset;

  constexpr auto operator<=>(const Utf8Error&) const noexcept = default;
};

struct DecodedUtf8 {
  char32_t code_point;
  ByteCount byte_count;

  constexpr auto operator<=>(const DecodedUtf8&) const noexcept = default;
};

using Utf8DecodeResult = std::expected<DecodedUtf8, Utf8Error>;
using Utf8ValidationResult = std::expected<void, Utf8Error>;
using Utf8CountResult = std::expected<std::size_t, Utf8Error>;

[[nodiscard]] Utf8DecodeResult DecodeUtf8(Utf8View value, ByteOffset offset) noexcept;
[[nodiscard]] Utf8ValidationResult ValidateUtf8(Utf8View value) noexcept;
[[nodiscard]] Utf8CountResult CountUtf8CodePoints(Utf8View value) noexcept;

namespace text_detail {

inline constexpr std::uint8_t kSpace = 0x01;
inline constexpr std::uint8_t kAlpha = 0x02;
inline constexpr std::uint8_t kDigit = 0x04;
inline constexpr std::uint8_t kHexDigit = 0x08;
inline constexpr std::uint8_t kLowercase = 0x20;
inline constexpr std::uint8_t kIdentifierExtra = 0x40;
inline constexpr std::uint8_t kQuote = 0x80;

[[nodiscard]] consteval std::array<std::uint8_t, 256> BuildSqliteCharacterClasses() {
  std::array<std::uint8_t, 256> classes{};
  for (std::size_t value = 0; value < classes.size(); ++value) {
    const auto byte = static_cast<std::uint8_t>(value);
    std::uint8_t flags = 0;

    if (byte == static_cast<std::uint8_t>(' ') ||
        (byte >= static_cast<std::uint8_t>('\t') && byte <= static_cast<std::uint8_t>('\r'))) {
      flags |= kSpace;
    }
    if ((byte >= static_cast<std::uint8_t>('A') && byte <= static_cast<std::uint8_t>('Z')) ||
        (byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('z'))) {
      flags |= kAlpha;
    }
    if (byte >= static_cast<std::uint8_t>('0') && byte <= static_cast<std::uint8_t>('9')) {
      flags |= kDigit | kHexDigit;
    }
    if ((byte >= static_cast<std::uint8_t>('A') && byte <= static_cast<std::uint8_t>('F')) ||
        (byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('f'))) {
      flags |= kHexDigit;
    }
    if (byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('z')) {
      flags |= kLowercase;
    }
    if (byte == static_cast<std::uint8_t>('_') || byte == static_cast<std::uint8_t>('$') ||
        byte >= 0x80U) {
      flags |= kIdentifierExtra;
    }
    if (byte == static_cast<std::uint8_t>('"') || byte == static_cast<std::uint8_t>('\'') ||
        byte == static_cast<std::uint8_t>('[') || byte == static_cast<std::uint8_t>('`')) {
      flags |= kQuote;
    }

    classes[value] = flags;
  }
  return classes;
}

inline constexpr auto kSqliteCharacterClasses = BuildSqliteCharacterClasses();

[[nodiscard]] constexpr bool HasClass(std::uint8_t value, std::uint8_t mask) noexcept {
  return (kSqliteCharacterClasses[value] & mask) != 0U;
}

}  // namespace text_detail

[[nodiscard]] constexpr bool IsSqliteSpace(std::uint8_t value) noexcept {
  return text_detail::HasClass(value, text_detail::kSpace);
}

[[nodiscard]] constexpr bool IsSqliteAlpha(std::uint8_t value) noexcept {
  return text_detail::HasClass(value, text_detail::kAlpha);
}

[[nodiscard]] constexpr bool IsSqliteDigit(std::uint8_t value) noexcept {
  return text_detail::HasClass(value, text_detail::kDigit);
}

[[nodiscard]] constexpr bool IsSqliteAlnum(std::uint8_t value) noexcept {
  return text_detail::HasClass(
      value, static_cast<std::uint8_t>(text_detail::kAlpha | text_detail::kDigit));
}

[[nodiscard]] constexpr bool IsSqliteHexDigit(std::uint8_t value) noexcept {
  return text_detail::HasClass(value, text_detail::kHexDigit);
}

[[nodiscard]] constexpr bool IsSqliteIdentifierByte(std::uint8_t value) noexcept {
  return text_detail::HasClass(value,
                               static_cast<std::uint8_t>(text_detail::kAlpha | text_detail::kDigit |
                                                         text_detail::kIdentifierExtra));
}

[[nodiscard]] constexpr bool IsSqliteQuote(std::uint8_t value) noexcept {
  return text_detail::HasClass(value, text_detail::kQuote);
}

[[nodiscard]] constexpr std::uint8_t SqliteToLower(std::uint8_t value) noexcept {
  if (value >= static_cast<std::uint8_t>('A') && value <= static_cast<std::uint8_t>('Z')) {
    return static_cast<std::uint8_t>(value + static_cast<std::uint8_t>('a' - 'A'));
  }
  return value;
}

[[nodiscard]] constexpr std::uint8_t SqliteToUpper(std::uint8_t value) noexcept {
  if (text_detail::HasClass(value, text_detail::kLowercase)) {
    return static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('a' - 'A'));
  }
  return value;
}

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TEXT_TEXT_HPP_
