#include "modern_sqlite/text/text.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace modern_sqlite {
namespace {

[[nodiscard]] std::uint8_t ByteAt(std::string_view value, std::size_t offset) noexcept {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(value[offset]));
}

[[nodiscard]] std::unexpected<Utf8Error> Utf8Failure(Utf8ErrorKind kind,
                                                     std::size_t offset) noexcept {
  return std::unexpected(Utf8Error{.kind = kind, .offset = ByteOffset{offset}});
}

}  // namespace

Utf8DecodeResult DecodeUtf8(Utf8View value, ByteOffset offset) noexcept {
  const std::string_view bytes = value.bytes();
  if (offset.value() > bytes.size()) {
    return Utf8Failure(Utf8ErrorKind::kOffsetOutOfRange, offset.value());
  }
  if (offset.value() == bytes.size()) {
    return Utf8Failure(Utf8ErrorKind::kEndOfInput, offset.value());
  }

  const std::uint8_t first = ByteAt(bytes, offset.value());
  if (first < 0x80U) {
    return DecodedUtf8{
        .code_point = static_cast<char32_t>(first),
        .byte_count = ByteCount{1},
    };
  }
  if (first < 0xc0U) {
    return Utf8Failure(Utf8ErrorKind::kUnexpectedContinuationByte, offset.value());
  }
  if (first < 0xc2U) {
    return Utf8Failure(Utf8ErrorKind::kOverlongEncoding, offset.value());
  }
  if (first > 0xf4U) {
    return Utf8Failure(Utf8ErrorKind::kCodePointOutOfRange, offset.value());
  }

  std::size_t width = 0;
  std::uint32_t code_point = 0;
  std::uint32_t minimum = 0;
  if (first <= 0xdfU) {
    width = 2;
    code_point = first & 0x1fU;
    minimum = 0x80U;
  } else if (first <= 0xefU) {
    width = 3;
    code_point = first & 0x0fU;
    minimum = 0x800U;
  } else {
    width = 4;
    code_point = first & 0x07U;
    minimum = 0x10000U;
  }

  const std::size_t remaining = bytes.size() - offset.value();
  for (std::size_t index = 1; index < width; ++index) {
    if (index >= remaining) {
      return Utf8Failure(Utf8ErrorKind::kTruncatedSequence, bytes.size());
    }
    const std::size_t byte_offset = offset.value() + index;
    const std::uint8_t continuation = ByteAt(bytes, byte_offset);
    if ((continuation & 0xc0U) != 0x80U) {
      return Utf8Failure(Utf8ErrorKind::kInvalidContinuationByte, byte_offset);
    }
    code_point = (code_point << 6U) | (continuation & 0x3fU);
  }

  if (code_point < minimum) {
    return Utf8Failure(Utf8ErrorKind::kOverlongEncoding, offset.value());
  }
  if (code_point >= 0xd800U && code_point <= 0xdfffU) {
    return Utf8Failure(Utf8ErrorKind::kSurrogateCodePoint, offset.value());
  }
  if (code_point > 0x10ffffU) {
    return Utf8Failure(Utf8ErrorKind::kCodePointOutOfRange, offset.value());
  }

  return DecodedUtf8{
      .code_point = static_cast<char32_t>(code_point),
      .byte_count = ByteCount{width},
  };
}

Utf8ValidationResult ValidateUtf8(Utf8View value) noexcept {
  std::size_t offset = 0;
  while (offset < value.size_bytes()) {
    const Utf8DecodeResult decoded = DecodeUtf8(value, ByteOffset{offset});
    if (!decoded.has_value()) {
      return std::unexpected(decoded.error());
    }
    offset += decoded->byte_count.value();
  }
  return {};
}

Utf8CountResult CountUtf8CodePoints(Utf8View value) noexcept {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < value.size_bytes()) {
    const Utf8DecodeResult decoded = DecodeUtf8(value, ByteOffset{offset});
    if (!decoded.has_value()) {
      return std::unexpected(decoded.error());
    }
    offset += decoded->byte_count.value();
    ++count;
  }
  return count;
}

}  // namespace modern_sqlite
