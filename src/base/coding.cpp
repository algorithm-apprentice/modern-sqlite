#include "modern_sqlite/base/coding.hpp"

#include <bit>
#include <limits>

namespace modern_sqlite {
namespace {

constexpr std::size_t kMaximumWalChecksumBytes = 65536;

[[nodiscard]] constexpr std::uint8_t ToUint8(std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::uint32_t LoadWalWord(std::span<const std::byte, sizeof(std::uint32_t)> input,
                                        WalChecksumByteOrder byte_order) noexcept {
  const auto big_endian = LoadBigEndian<std::uint32_t>(input);
  if (byte_order == WalChecksumByteOrder::kBigEndian) {
    return big_endian;
  }
  return std::byteswap(big_endian);
}

}  // namespace

CodingResult<DecodedVarint> DecodeSqliteVarint(ByteView input) noexcept {
  if (input.empty()) {
    return std::unexpected(CodingError::kInputTooShort);
  }

  const std::uint8_t first = ToUint8(input.front());
  if (first < 0x80U) {
    return DecodedVarint{.value = first, .bytes_consumed = ByteCount{1}};
  }

  std::uint64_t value = first & 0x7fU;
  for (std::size_t index = 1; index < 8; ++index) {
    if (index >= input.size()) {
      return std::unexpected(CodingError::kInputTooShort);
    }

    const std::uint8_t byte = ToUint8(input[index]);
    value = (value << 7U) | (byte & 0x7fU);
    if (byte < 0x80U) {
      return DecodedVarint{
          .value = value,
          .bytes_consumed = ByteCount{index + 1},
      };
    }
  }

  if (input.size() < 9) {
    return std::unexpected(CodingError::kInputTooShort);
  }

  value = (value << 8U) | ToUint8(input[8]);
  return DecodedVarint{.value = value, .bytes_consumed = ByteCount{9}};
}

CodingResult<ByteCount> EncodeSqliteVarint(std::uint64_t value, MutableByteView output) noexcept {
  const ByteCount encoded_size = SqliteVarintLength(value);
  if (output.size() < encoded_size.value()) {
    return std::unexpected(CodingError::kOutputTooSmall);
  }

  if (encoded_size.value() == 9) {
    output[8] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
    for (std::size_t index = 8; index > 0; --index) {
      output[index - 1] = static_cast<std::byte>((value & 0x7fU) | 0x80U);
      value >>= 7U;
    }
    return encoded_size;
  }

  for (std::size_t index = encoded_size.value(); index > 0; --index) {
    auto byte = static_cast<std::uint8_t>(value & 0x7fU);
    if (index != encoded_size.value()) {
      byte |= 0x80U;
    }
    output[index - 1] = static_cast<std::byte>(byte);
    value >>= 7U;
  }
  return encoded_size;
}

CodingResult<std::int64_t> CheckedAdd(std::int64_t lhs, std::int64_t rhs) noexcept {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if ((rhs > 0 && lhs > kMax - rhs) || (rhs < 0 && lhs < kMin - rhs)) {
    return std::unexpected(CodingError::kOverflow);
  }
  return lhs + rhs;
}

CodingResult<std::int64_t> CheckedSubtract(std::int64_t lhs, std::int64_t rhs) noexcept {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if ((rhs > 0 && lhs < kMin + rhs) || (rhs < 0 && lhs > kMax + rhs)) {
    return std::unexpected(CodingError::kOverflow);
  }
  return lhs - rhs;
}

CodingResult<std::int64_t> CheckedMultiply(std::int64_t lhs, std::int64_t rhs) noexcept {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if ((lhs == -1 && rhs == kMin) || (rhs == -1 && lhs == kMin)) {
    return std::unexpected(CodingError::kOverflow);
  }
  if (lhs > 0) {
    if ((rhs > 0 && lhs > kMax / rhs) || (rhs < 0 && rhs < kMin / lhs)) {
      return std::unexpected(CodingError::kOverflow);
    }
  } else if ((rhs > 0 && lhs < kMin / rhs) || (rhs < 0 && lhs < kMax / rhs)) {
    return std::unexpected(CodingError::kOverflow);
  }
  return lhs * rhs;
}

std::uint32_t ComputeRollbackJournalChecksum(ByteView page, std::uint32_t seed) noexcept {
  if (page.size() <= 200) {
    return seed;
  }

  std::size_t offset = page.size() - 200;
  while (offset > 0) {
    seed += ToUint8(page[offset]);
    if (offset <= 200) {
      break;
    }
    offset -= 200;
  }
  return seed;
}

CodingResult<WalChecksum> ComputeWalChecksum(ByteView input, WalChecksumByteOrder byte_order,
                                             WalChecksum initial) noexcept {
  if (input.empty() || input.size() > kMaximumWalChecksumBytes || input.size() % 8 != 0) {
    return std::unexpected(CodingError::kInvalidChecksumLength);
  }

  for (std::size_t offset = 0; offset < input.size(); offset += 8) {
    const ByteView block = input.subspan(offset, 8);
    const std::uint32_t first_word = LoadWalWord(block.first<sizeof(std::uint32_t)>(), byte_order);
    const std::uint32_t second_word =
        LoadWalWord(block.subspan<sizeof(std::uint32_t), sizeof(std::uint32_t)>(), byte_order);
    initial.first += first_word + initial.second;
    initial.second += second_word + initial.first;
  }
  return initial;
}

}  // namespace modern_sqlite
