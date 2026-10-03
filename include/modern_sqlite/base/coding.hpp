#ifndef MODERN_SQLITE_BASE_CODING_HPP_
#define MODERN_SQLITE_BASE_CODING_HPP_

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

#include "modern_sqlite/base/bytes.hpp"

namespace modern_sqlite {

enum class CodingError {
  kInputTooShort,
  kOutputTooSmall,
  kInvalidChecksumLength,
  kOverflow,
};

template <typename T>
using CodingResult = std::expected<T, CodingError>;

template <typename T>
concept FixedWidthUnsignedInteger =
    std::same_as<T, std::uint16_t> || std::same_as<T, std::uint32_t> ||
    std::same_as<T, std::uint64_t>;

template <FixedWidthUnsignedInteger T>
[[nodiscard]] inline T LoadBigEndian(std::span<const std::byte, sizeof(T)> input) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    T value;
    std::memcpy(&value, input.data(), sizeof(value));
    return std::byteswap(value);
  } else if constexpr (std::endian::native == std::endian::big) {
    T value;
    std::memcpy(&value, input.data(), sizeof(value));
    return value;
  } else {
    T value = 0;
    for (const std::byte byte : input) {
      value = static_cast<T>((value << 8U) | std::to_integer<std::uint8_t>(byte));
    }
    return value;
  }
}

template <FixedWidthUnsignedInteger T>
inline void StoreBigEndian(std::span<std::byte, sizeof(T)> output, T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    const T encoded = std::byteswap(value);
    std::memcpy(output.data(), &encoded, sizeof(encoded));
  } else if constexpr (std::endian::native == std::endian::big) {
    std::memcpy(output.data(), &value, sizeof(value));
  } else {
    for (std::size_t index = sizeof(T); index > 0; --index) {
      output[index - 1] = static_cast<std::byte>(value & static_cast<T>(0xffU));
      value >>= 8U;
    }
  }
}

template <FixedWidthUnsignedInteger T>
[[nodiscard]] inline CodingResult<T> ReadBigEndian(ByteView input) noexcept {
  if (input.size() < sizeof(T)) {
    return std::unexpected(CodingError::kInputTooShort);
  }
  return LoadBigEndian<T>(input.template first<sizeof(T)>());
}

template <FixedWidthUnsignedInteger T>
[[nodiscard]] inline CodingResult<void> WriteBigEndian(MutableByteView output, T value) noexcept {
  if (output.size() < sizeof(T)) {
    return std::unexpected(CodingError::kOutputTooSmall);
  }
  StoreBigEndian<T>(output.template first<sizeof(T)>(), value);
  return {};
}

struct DecodedVarint {
  std::uint64_t value;
  ByteCount bytes_consumed;

  constexpr bool operator==(const DecodedVarint&) const noexcept = default;
};

[[nodiscard]] constexpr ByteCount SqliteVarintLength(std::uint64_t value) noexcept {
  if (value >= (std::uint64_t{1} << 56U)) {
    return ByteCount{9};
  }

  std::size_t length = 1;
  while (value > 0x7fU) {
    value >>= 7U;
    ++length;
  }
  return ByteCount{length};
}

[[nodiscard]] CodingResult<DecodedVarint> DecodeSqliteVarint(ByteView input) noexcept;
[[nodiscard]] CodingResult<ByteCount> EncodeSqliteVarint(std::uint64_t value,
                                                         MutableByteView output) noexcept;

[[nodiscard]] CodingResult<std::int64_t> CheckedAdd(std::int64_t lhs, std::int64_t rhs) noexcept;
[[nodiscard]] CodingResult<std::int64_t> CheckedSubtract(std::int64_t lhs,
                                                         std::int64_t rhs) noexcept;
[[nodiscard]] CodingResult<std::int64_t> CheckedMultiply(std::int64_t lhs,
                                                         std::int64_t rhs) noexcept;

[[nodiscard]] std::uint32_t ComputeRollbackJournalChecksum(ByteView page,
                                                           std::uint32_t seed) noexcept;

enum class WalChecksumByteOrder {
  kLittleEndian,
  kBigEndian,
};

struct WalChecksum {
  std::uint32_t first = 0;
  std::uint32_t second = 0;

  constexpr bool operator==(const WalChecksum&) const noexcept = default;
};

[[nodiscard]] CodingResult<WalChecksum> ComputeWalChecksum(ByteView input,
                                                           WalChecksumByteOrder byte_order,
                                                           WalChecksum initial = {}) noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BASE_CODING_HPP_
