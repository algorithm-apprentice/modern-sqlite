#include "sqlite_float.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace modern_sqlite::internal {
namespace {

// These integer conversion routines follow the pinned SQLite 3.54.0 util.c
// algorithms so SQL-visible rounding and rendering do not depend on libc.
using Uint32 = std::uint32_t;
using Uint64 = std::uint64_t;

constexpr int kPowersOfTenFirst = -348;
constexpr int kPowersOfTenLast = 347;
constexpr int kDigitBufferSize = 20;

struct Product128 {
  Uint64 high;
  Uint64 low;
};

struct ProductOperands {
  Uint64 left;
  Uint64 right;
};

struct Uint96 {
  Uint64 high;
  Uint32 low;
};

[[nodiscard]] Product128 Multiply128(ProductOperands operands) noexcept {
  const Uint64 left = operands.left;
  const Uint64 right = operands.right;
  const Uint64 left_low = static_cast<Uint32>(left);
  const Uint64 left_high = left >> 32U;
  const Uint64 right_low = static_cast<Uint32>(right);
  const Uint64 right_high = right >> 32U;
  const Uint64 low_product = left_low * right_low;
  const Uint64 high_product = left_high * right_high;
  const Uint64 cross_left = left_low * right_high;
  const Uint64 cross_right = left_high * right_low;
  const Uint64 middle =
      (low_product >> 32U) + static_cast<Uint32>(cross_left) + static_cast<Uint32>(cross_right);
  return Product128{
      .high = high_product + (cross_left >> 32U) + (cross_right >> 32U) + (middle >> 32U),
      .low = (low_product & UINT64_C(0xffffffff)) | (middle << 32U),
  };
}

[[nodiscard]] Uint96 Multiply160(Uint96 multiplicand, Uint64 multiplier) noexcept {
  const Uint64 x2 = multiplicand.high >> 32U;
  const Uint64 x1 = multiplicand.high & UINT64_C(0xffffffff);
  const Uint64 x0 = multiplicand.low;
  const Uint64 y1 = multiplier >> 32U;
  const Uint64 y0 = multiplier & UINT64_C(0xffffffff);
  const Uint64 x2y1 = x2 * y1;
  const Uint64 result4 = x2y1 >> 32U;
  const Uint64 x2y0 = x2 * y0;
  const Uint64 x1y1 = x1 * y1;
  Uint64 result3 = (x2y1 & UINT64_C(0xffffffff)) + (x2y0 >> 32U) + (x1y1 >> 32U);
  const Uint64 x1y0 = x1 * y0;
  const Uint64 x0y1 = x0 * y1;
  Uint64 result2 =
      (x2y0 & UINT64_C(0xffffffff)) + (x1y1 & UINT64_C(0xffffffff)) + (x1y0 >> 32U) + (x0y1 >> 32U);
  const Uint64 x0y0 = x0 * y0;
  const Uint64 result1 =
      (x1y0 & UINT64_C(0xffffffff)) + (x0y1 & UINT64_C(0xffffffff)) + (x0y0 >> 32U);
  result2 += result1 >> 32U;
  result3 += result2 >> 32U;
  return Uint96{
      .high = (result4 << 32U) + result3,
      .low = static_cast<Uint32>(result2),
  };
}

[[nodiscard]] Uint64 PowerOfTen(int power, Uint32& low) noexcept {
  static constexpr std::array<Uint64, 27> kBase{
      UINT64_C(0x8000000000000000), UINT64_C(0xa000000000000000), UINT64_C(0xc800000000000000),
      UINT64_C(0xfa00000000000000), UINT64_C(0x9c40000000000000), UINT64_C(0xc350000000000000),
      UINT64_C(0xf424000000000000), UINT64_C(0x9896800000000000), UINT64_C(0xbebc200000000000),
      UINT64_C(0xee6b280000000000), UINT64_C(0x9502f90000000000), UINT64_C(0xba43b74000000000),
      UINT64_C(0xe8d4a51000000000), UINT64_C(0x9184e72a00000000), UINT64_C(0xb5e620f480000000),
      UINT64_C(0xe35fa931a0000000), UINT64_C(0x8e1bc9bf04000000), UINT64_C(0xb1a2bc2ec5000000),
      UINT64_C(0xde0b6b3a76400000), UINT64_C(0x8ac7230489e80000), UINT64_C(0xad78ebc5ac620000),
      UINT64_C(0xd8d726b7177a8000), UINT64_C(0x878678326eac9000), UINT64_C(0xa968163f0a57b400),
      UINT64_C(0xd3c21bcecceda100), UINT64_C(0x84595161401484a0), UINT64_C(0xa56fa5b99019a5c8),
  };
  static constexpr std::array<Uint64, 26> kScale{
      UINT64_C(0x8049a4ac0c5811ae), UINT64_C(0xcf42894a5dce35ea), UINT64_C(0xa76c582338ed2621),
      UINT64_C(0x873e4f75e2224e68), UINT64_C(0xda7f5bf590966848), UINT64_C(0xb080392cc4349dec),
      UINT64_C(0x8e938662882af53e), UINT64_C(0xe65829b3046b0afa), UINT64_C(0xba121a4650e4ddeb),
      UINT64_C(0x964e858c91ba2655), UINT64_C(0xf2d56790ab41c2a2), UINT64_C(0xc428d05aa4751e4c),
      UINT64_C(0x9e74d1b791e07e48), UINT64_C(0xcccccccccccccccc), UINT64_C(0xcecb8f27f4200f3a),
      UINT64_C(0xa70c3c40a64e6c51), UINT64_C(0x86f0ac99b4e8dafd), UINT64_C(0xda01ee641a708de9),
      UINT64_C(0xb01ae745b101e9e4), UINT64_C(0x8e41ade9fbebc27d), UINT64_C(0xe5d3ef282a242e81),
      UINT64_C(0xb9a74a0637ce2ee1), UINT64_C(0x95f83d0a1fb69cd9), UINT64_C(0xf24a01a73cf2dccf),
      UINT64_C(0xc3b8358109e84f07), UINT64_C(0x9e19db92b4e31ba9),
  };
  static constexpr std::array<Uint32, 26> kScaleLow{
      UINT32_C(0x205b896d), UINT32_C(0x52064cad), UINT32_C(0xaf2af2b8), UINT32_C(0x5a7744a7),
      UINT32_C(0xaf39a475), UINT32_C(0xbd8d794e), UINT32_C(0x547eb47b), UINT32_C(0x0cb4a5a3),
      UINT32_C(0x92f34d62), UINT32_C(0x3a6a07f9), UINT32_C(0xfae27299), UINT32_C(0xaa97e14c),
      UINT32_C(0x775ea265), UINT32_C(0xcccccccc), UINT32_C(0x00000000), UINT32_C(0x999090b6),
      UINT32_C(0x69a028bb), UINT32_C(0xe80e6f48), UINT32_C(0x5ec05dd0), UINT32_C(0x14588f14),
      UINT32_C(0x8f1668c9), UINT32_C(0x6d953e2c), UINT32_C(0x4abdaf10), UINT32_C(0xbc633b39),
      UINT32_C(0x0a862f81), UINT32_C(0x6c07a2c2),
  };

  int group = 0;
  int remainder = 0;
  if (power < 0) {
    if (power == -1) {
      low = kScaleLow[13];
      return kScale[13];
    }
    group = power / 27;
    remainder = power % 27;
    if (remainder != 0) {
      --group;
      remainder += 27;
    }
  } else if (power < 27) {
    low = 0;
    return kBase[static_cast<std::size_t>(power)];
  } else {
    group = power / 27;
    remainder = power % 27;
  }

  const int signed_scale_index = group + 13;
  const auto scale_index = static_cast<std::size_t>(signed_scale_index);
  const Uint64 scale = kScale[scale_index];
  if (remainder == 0) {
    low = kScaleLow[scale_index];
    return scale;
  }

  Uint96 product = Multiply160(Uint96{.high = scale, .low = kScaleLow[scale_index]},
                               kBase[static_cast<std::size_t>(remainder)]);
  if ((product.high & (UINT64_C(1) << 63U)) == 0) {
    product.high = (product.high << 1U) | ((product.low >> 31U) & 1U);
    product.low = (product.low << 1U) | 1U;
  }
  low = product.low;
  return product.high;
}

[[nodiscard]] constexpr int FloorDivide(int numerator, int denominator) noexcept {
  const int quotient = numerator / denominator;
  const int remainder = numerator % denominator;
  return quotient - (remainder != 0 && numerator < 0 ? 1 : 0);
}

[[nodiscard]] constexpr int Power10To2(int power) noexcept {
  return FloorDivide(power * 108853, 32768);
}

[[nodiscard]] constexpr int Power2To10(int power) noexcept {
  return FloorDivide(power * 78913, 262144);
}

struct BinaryValue {
  Uint64 mantissa;
  int exponent;
};

struct DecimalValue {
  Uint64 mantissa;
  int exponent;
};

[[nodiscard]] DecimalValue ConvertBinaryToDecimal(BinaryValue value, int digit_count) noexcept {
  const int power = digit_count - 1 - Power2To10(value.exponent + 63);
  Uint32 power_low = 0;
  Uint64 high = Multiply128(ProductOperands{
                                .left = value.mantissa,
                                .right = PowerOfTen(power, power_low),
                            })
                    .high;
  if (digit_count == 18) {
    const auto shift = static_cast<unsigned>(-(value.exponent + Power10To2(power) + 2));
    high >>= shift;
    return DecimalValue{
        .mantissa = (high + ((high << 1U) & 2U)) >> 1U,
        .exponent = -power,
    };
  }
  const auto shift = static_cast<unsigned>(-(value.exponent + Power10To2(power) + 1));
  return DecimalValue{
      .mantissa = high >> shift,
      .exponent = -power,
  };
}

[[nodiscard]] double ConvertDecimalToBinary(DecimalValue value) noexcept {
  if (value.exponent < kPowersOfTenFirst) {
    return 0.0;
  }
  if (value.exponent > kPowersOfTenLast) {
    return std::numeric_limits<double>::infinity();
  }

  const int bit_count = 64 - std::countl_zero(value.mantissa);
  const int binary_power = Power10To2(value.exponent);
  int exponent = 53 - bit_count - binary_power;
  if (exponent > 1074) {
    if (exponent >= 1130) {
      return 0.0;
    }
    exponent = 1074;
  }

  const auto shift = static_cast<unsigned>(-(exponent - (64 - bit_count) + binary_power + 3));
  Uint32 power_low = 0;
  Uint64 power_high = PowerOfTen(value.exponent, power_low);
  if (power_low != 0) {
    ++power_high;
    power_low = ~power_low;
  }

  const auto normalization_shift = static_cast<unsigned>(64 - bit_count);
  const Uint64 shifted_decimal = value.mantissa << normalization_shift;
  const Product128 product =
      Multiply128(ProductOperands{.left = shifted_decimal, .right = power_high});
  Uint64 high = product.high;
  const auto middle_high = static_cast<Uint32>(product.low >> 32U);
  Uint64 sticky = 1;
  if ((high & ((UINT64_C(1) << shift) - 1U)) == 0) {
    const auto middle_low =
        static_cast<Uint32>(Multiply128(ProductOperands{
                                            .left = shifted_decimal,
                                            .right = static_cast<Uint64>(power_low) << 32U,
                                        })
                                .high >>
                            32U);
    sticky = middle_high - middle_low > 1U ? 1U : 0U;
    high -= middle_high < middle_low ? 1U : 0U;
  }

  Uint64 rounded = (high >> shift) | sticky;
  const int adjust = rounded >= (UINT64_C(1) << 55U) - 2U ? 1 : 0;
  if (adjust != 0) {
    rounded = (rounded >> 1U) | (rounded & 1U);
    --exponent;
  }
  Uint64 ieee_mantissa = (rounded + 1U + ((rounded >> 2U) & 1U)) >> 2U;
  if (exponent <= -972) {
    return std::numeric_limits<double>::infinity();
  }
  if ((ieee_mantissa & (UINT64_C(1) << 52U)) != 0) {
    ieee_mantissa =
        (ieee_mantissa & ~(UINT64_C(1) << 52U)) | (static_cast<Uint64>(1075 - exponent) << 52U);
  }
  return std::bit_cast<double>(ieee_mantissa);
}

[[nodiscard]] constexpr int AdjustDecimalExponent(int exponent, int delta) noexcept {
  return std::clamp(exponent + delta, -10'000, 10'000);
}

struct DecimalDecode {
  std::array<char, kDigitBufferSize + 1> buffer{};
  std::size_t offset = 0;
  int digit_count = 0;
  int decimal_point = 0;
  bool negative = false;

  [[nodiscard]] std::string_view digits() const noexcept {
    return {buffer.data() + offset, static_cast<std::size_t>(digit_count)};
  }
};

[[nodiscard]] DecimalDecode DecodeReal(double input) noexcept {
  DecimalDecode result;
  double value = input;
  if (value < 0.0) {
    result.negative = true;
    value = -value;
  } else if (value == 0.0) {
    result.buffer[0] = '0';
    result.digit_count = 1;
    result.decimal_point = 1;
    return result;
  }

  const auto bits = std::bit_cast<Uint64>(value);
  int exponent = static_cast<int>((bits >> 52U) & UINT64_C(0x7ff));
  Uint64 mantissa = bits & UINT64_C(0x000fffffffffffff);
  if (exponent == 0) {
    const int leading_zeros = std::countl_zero(mantissa);
    mantissa <<= static_cast<unsigned>(leading_zeros);
    exponent = -1074 - leading_zeros;
  } else {
    mantissa = (mantissa << 11U) | (UINT64_C(1) << 63U);
    exponent -= 1086;
  }

  const DecimalValue decimal =
      ConvertBinaryToDecimal(BinaryValue{.mantissa = mantissa, .exponent = exponent}, 18);
  Uint64 decimal_mantissa = decimal.mantissa;

  std::size_t index = kDigitBufferSize;
  while (decimal_mantissa >= 10U) {
    result.buffer[--index] = static_cast<char>('0' + (decimal_mantissa % 10U));
    decimal_mantissa /= 10U;
  }
  result.buffer[--index] = static_cast<char>('0' + decimal_mantissa);

  int digit_count = kDigitBufferSize - static_cast<int>(index);
  result.decimal_point = digit_count + decimal.exponent;
  int rounded_digits = 17;
  if (rounded_digits < digit_count) {
    char* const digits = result.buffer.data() + index;
    if (digits[15] == '9' && digits[14] == '9') {
      int prefix_digits = 14;
      while (prefix_digits > 0 && digits[prefix_digits - 1] == '9') {
        --prefix_digits;
      }
      Uint64 candidate = 1;
      if (prefix_digits != 0) {
        candidate = static_cast<Uint64>(digits[0] - '0');
        for (int position = 1; position < prefix_digits; ++position) {
          candidate = (candidate * 10U) + static_cast<Uint64>(digits[position] - '0');
        }
        ++candidate;
      }
      if (value == ConvertDecimalToBinary(DecimalValue{
                       .mantissa = candidate,
                       .exponent = decimal.exponent + digit_count - prefix_digits,
                   })) {
        rounded_digits = prefix_digits + 1;
      }
    } else if (result.decimal_point >= digit_count ||
               (digits[15] == '0' && digits[14] == '0' && digits[13] == '0')) {
      int prefix_digits = 13;
      while (digits[prefix_digits - 1] == '0') {
        --prefix_digits;
      }
      auto candidate = static_cast<Uint64>(digits[0] - '0');
      for (int position = 1; position < prefix_digits; ++position) {
        candidate = (candidate * 10U) + static_cast<Uint64>(digits[position] - '0');
      }
      if (value == ConvertDecimalToBinary(DecimalValue{
                       .mantissa = candidate,
                       .exponent = decimal.exponent + digit_count - prefix_digits,
                   })) {
        rounded_digits = prefix_digits + 1;
      }
    }

    digit_count = rounded_digits;
    if (digits[rounded_digits] >= '5') {
      int position = rounded_digits - 1;
      while (true) {
        ++digits[position];
        if (digits[position] <= '9') {
          break;
        }
        digits[position] = '0';
        if (position == 0) {
          --index;
          result.buffer[index] = '1';
          ++digit_count;
          ++result.decimal_point;
          break;
        }
        --position;
      }
    }
  }

  while (result.buffer[index + static_cast<std::size_t>(digit_count) - 1U] == '0') {
    --digit_count;
  }
  result.offset = index;
  result.digit_count = digit_count;
  return result;
}

void AppendExponent(std::string& output, int exponent) {
  output.push_back('e');
  if (exponent < 0) {
    output.push_back('-');
    exponent = -exponent;
  } else {
    output.push_back('+');
  }

  std::array<char, 4> buffer{};
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), exponent);
  if (converted.ec != std::errc{}) {
    std::terminate();
  }
  const auto digit_count = static_cast<std::size_t>(converted.ptr - buffer.data());
  if (digit_count == 1U) {
    output.push_back('0');
  }
  output.append(buffer.data(), digit_count);
}

}  // namespace

double ParseSqliteReal(std::string_view token) noexcept {
  std::size_t position = 0;
  bool negative = false;
  if (position < token.size() && (token[position] == '+' || token[position] == '-')) {
    negative = token[position] == '-';
    ++position;
  }

  constexpr Uint64 kMantissaLimit = (std::numeric_limits<Uint64>::max() - 9U) / 10U;
  Uint64 mantissa = 0;
  int decimal_exponent = 0;
  while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
    const auto digit = static_cast<Uint64>(token[position] - '0');
    mantissa = (mantissa * 10U) + digit;
    ++position;
    if (mantissa >= kMantissaLimit) {
      while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
        ++position;
        decimal_exponent = AdjustDecimalExponent(decimal_exponent, 1);
      }
      break;
    }
  }

  if (position < token.size() && token[position] == '.') {
    ++position;
    while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
      if (mantissa < kMantissaLimit) {
        mantissa = (mantissa * 10U) + static_cast<Uint64>(token[position] - '0');
        decimal_exponent = AdjustDecimalExponent(decimal_exponent, -1);
      }
      ++position;
    }
  }

  if (position < token.size() && (token[position] == 'e' || token[position] == 'E')) {
    ++position;
    bool negative_exponent = false;
    if (position < token.size() && (token[position] == '+' || token[position] == '-')) {
      negative_exponent = token[position] == '-';
      ++position;
    }
    int explicit_exponent = 0;
    while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
      explicit_exponent =
          explicit_exponent < 10'000 ? (explicit_exponent * 10) + (token[position] - '0') : 10'000;
      ++position;
    }
    decimal_exponent = AdjustDecimalExponent(
        decimal_exponent, negative_exponent ? -explicit_exponent : explicit_exponent);
  }

  double result = mantissa == 0 ? 0.0
                                : ConvertDecimalToBinary(DecimalValue{
                                      .mantissa = mantissa, .exponent = decimal_exponent});
  if (negative) {
    result = -result;
  }
  return result;
}

std::string FormatSqliteReal(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return std::signbit(value) ? "-Inf" : "Inf";
  }

  const DecimalDecode decoded = DecodeReal(value);
  const std::string_view digits = decoded.digits();
  std::string output;
  output.reserve(32);
  if (decoded.negative) {
    output.push_back('-');
  }

  const int exponent = decoded.decimal_point - 1;
  if (exponent < -4 || exponent > 16) {
    output.push_back(digits.front());
    output.push_back('.');
    if (digits.size() == 1U) {
      output.push_back('0');
    } else {
      output.append(digits.substr(1));
    }
    AppendExponent(output, exponent);
    return output;
  }

  if (decoded.decimal_point <= 0) {
    output.append("0.");
    output.append(static_cast<std::size_t>(-decoded.decimal_point), '0');
    output.append(digits);
  } else if (decoded.decimal_point >= decoded.digit_count) {
    output.append(digits);
    output.append(static_cast<std::size_t>(decoded.decimal_point - decoded.digit_count), '0');
    output.append(".0");
  } else {
    const auto decimal_offset = static_cast<std::size_t>(decoded.decimal_point);
    output.append(digits.substr(0, decimal_offset));
    output.push_back('.');
    output.append(digits.substr(decimal_offset));
  }
  return output;
}

}  // namespace modern_sqlite::internal
