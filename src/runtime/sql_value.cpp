#include "modern_sqlite/runtime/sql_value.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "sqlite_float.hpp"

namespace modern_sqlite {

class SqlValueAccess {
 public:
  [[nodiscard]] static std::int64_t Integer(const SqlValue& value) noexcept {
    const auto* result = std::get_if<std::int64_t>(&value.storage_);
    if (result == nullptr) {
      std::terminate();
    }
    return *result;
  }

  [[nodiscard]] static double Real(const SqlValue& value) noexcept {
    const auto* result = std::get_if<double>(&value.storage_);
    if (result == nullptr) {
      std::terminate();
    }
    return *result;
  }

  [[nodiscard]] static Utf8View Text(const SqlValue& value) noexcept {
    const auto* result = std::get_if<std::string>(&value.storage_);
    if (result == nullptr) {
      std::terminate();
    }
    return Utf8View{*result};
  }

  [[nodiscard]] static ByteView Blob(const SqlValue& value) noexcept {
    const auto* result = std::get_if<ByteBuffer>(&value.storage_);
    if (result == nullptr) {
      std::terminate();
    }
    return result->view();
  }
};

namespace {

constexpr double kInt64LowerBound = -9223372036854775808.0;
constexpr double kInt64UpperBound = 9223372036854775808.0;
constexpr std::int64_t kExactNumericLowerBound = -2251799813685248LL;
constexpr std::int64_t kExactNumericUpperBound = 2251799813685248LL;

struct IntegerPrefix {
  std::int64_t value = 0;
  std::size_t end = 0;
  bool has_digits = false;
  bool overflow = false;
};

struct DecimalPrefix {
  std::size_t begin = 0;
  std::size_t end = 0;
  bool has_digits = false;
  bool has_real_syntax = false;
};

[[nodiscard]] constexpr bool IsDigit(char value) noexcept { return value >= '0' && value <= '9'; }

[[nodiscard]] std::size_t SkipSqliteSpaces(std::string_view input, std::size_t offset) noexcept {
  while (offset < input.size() && IsSqliteSpace(static_cast<std::uint8_t>(input[offset]))) {
    ++offset;
  }
  return offset;
}

[[nodiscard]] std::string_view BeforeFirstNull(std::string_view input) noexcept {
  const std::size_t null_offset = input.find('\0');
  if (null_offset != std::string_view::npos) {
    input.remove_suffix(input.size() - null_offset);
  }
  return input;
}

[[nodiscard]] IntegerPrefix ParseIntegerPrefix(std::string_view input) noexcept {
  std::size_t position = SkipSqliteSpaces(input, 0);
  bool negative = false;
  if (position < input.size() && (input[position] == '+' || input[position] == '-')) {
    negative = input[position] == '-';
    ++position;
  }

  const std::uint64_t limit =
      negative ? (std::uint64_t{1} << 63U)
               : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  std::uint64_t magnitude = 0;
  bool overflow = false;
  const std::size_t digit_begin = position;
  while (position < input.size() && IsDigit(input[position])) {
    const auto digit = static_cast<std::uint64_t>(input[position] - '0');
    if (magnitude > (limit - digit) / 10U) {
      overflow = true;
    } else if (!overflow) {
      magnitude = (magnitude * 10U) + digit;
    }
    ++position;
  }

  if (position == digit_begin) {
    return IntegerPrefix{.end = position};
  }
  if (overflow) {
    return IntegerPrefix{
        .value = negative ? std::numeric_limits<std::int64_t>::min()
                          : std::numeric_limits<std::int64_t>::max(),
        .end = position,
        .has_digits = true,
        .overflow = true,
    };
  }

  std::int64_t value = 0;
  if (negative && magnitude == (std::uint64_t{1} << 63U)) {
    value = std::numeric_limits<std::int64_t>::min();
  } else if (negative) {
    value = -static_cast<std::int64_t>(magnitude);
  } else {
    value = static_cast<std::int64_t>(magnitude);
  }
  return IntegerPrefix{
      .value = value,
      .end = position,
      .has_digits = true,
      .overflow = false,
  };
}

[[nodiscard]] DecimalPrefix ScanDecimalPrefix(std::string_view input) noexcept {
  std::size_t position = SkipSqliteSpaces(input, 0);
  const std::size_t begin = position;
  if (position < input.size() && (input[position] == '+' || input[position] == '-')) {
    ++position;
  }

  std::size_t digit_count = 0;
  while (position < input.size() && IsDigit(input[position])) {
    ++digit_count;
    ++position;
  }

  bool has_real_syntax = false;
  if (position < input.size() && input[position] == '.') {
    has_real_syntax = true;
    ++position;
    while (position < input.size() && IsDigit(input[position])) {
      ++digit_count;
      ++position;
    }
  }
  if (digit_count == 0) {
    return DecimalPrefix{.begin = begin, .end = begin};
  }

  if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
    std::size_t exponent_position = position + 1U;
    if (exponent_position < input.size() &&
        (input[exponent_position] == '+' || input[exponent_position] == '-')) {
      ++exponent_position;
    }
    const std::size_t exponent_digits = exponent_position;
    while (exponent_position < input.size() && IsDigit(input[exponent_position])) {
      ++exponent_position;
    }
    if (exponent_position != exponent_digits) {
      has_real_syntax = true;
      position = exponent_position;
    }
  }

  return DecimalPrefix{
      .begin = begin,
      .end = position,
      .has_digits = true,
      .has_real_syntax = has_real_syntax,
  };
}

[[nodiscard]] double ParseDouble(std::string_view input, const DecimalPrefix& prefix) noexcept {
  input.remove_prefix(prefix.begin);
  input.remove_suffix(input.size() - (prefix.end - prefix.begin));
  return internal::ParseSqliteReal(input);
}

[[nodiscard]] std::int64_t TruncateRealToInteger(double value) noexcept {
  if (value <= kInt64LowerBound) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (value >= kInt64UpperBound) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::optional<std::int64_t> AffinityInteger(double value) noexcept {
  const std::int64_t integer = TruncateRealToInteger(value);
  if (integer == std::numeric_limits<std::int64_t>::min() ||
      integer == std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt;
  }
  if (value == static_cast<double>(integer)) {
    return integer;
  }
  return std::nullopt;
}

[[nodiscard]] bool RealSameAsInteger(double real, std::int64_t integer) noexcept {
  return real == 0.0 || (real == static_cast<double>(integer) &&
                         integer >= kExactNumericLowerBound && integer < kExactNumericUpperBound);
}

[[nodiscard]] bool IsCompleteDecimal(std::string_view input, const DecimalPrefix& prefix) noexcept {
  return prefix.has_digits && SkipSqliteSpaces(input, prefix.end) == input.size();
}

[[nodiscard]] std::optional<SqlValue> TryApplyNumericTextAffinity(std::string_view bytes,
                                                                  bool force_real) {
  const IntegerPrefix integer = ParseIntegerPrefix(bytes);
  if (integer.has_digits && !integer.overflow &&
      SkipSqliteSpaces(bytes, integer.end) == bytes.size()) {
    return force_real ? SqlValue::Real(static_cast<double>(integer.value))
                      : SqlValue::Integer(integer.value);
  }

  const std::string_view numeric_input = BeforeFirstNull(bytes);
  const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
  if (!IsCompleteDecimal(numeric_input, prefix)) {
    return std::nullopt;
  }
  const double real = ParseDouble(numeric_input, prefix);
  if (const auto exact_integer = AffinityInteger(real); exact_integer.has_value()) {
    return force_real ? SqlValue::Real(static_cast<double>(*exact_integer))
                      : SqlValue::Integer(*exact_integer);
  }
  return SqlValue::Real(real);
}

[[nodiscard]] SqlValue ParseForcedNumeric(std::string_view bytes) {
  const std::string_view numeric_input = BeforeFirstNull(bytes);
  const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
  if (!prefix.has_digits) {
    return SqlValue::Integer(0);
  }

  const double real = ParseDouble(numeric_input, prefix);
  if (!prefix.has_real_syntax) {
    const IntegerPrefix integer = ParseIntegerPrefix(bytes);
    if (integer.has_digits && !integer.overflow) {
      return SqlValue::Integer(integer.value);
    }
  }

  const std::int64_t integer = TruncateRealToInteger(real);
  if (RealSameAsInteger(real, integer)) {
    return SqlValue::Integer(integer);
  }
  return SqlValue::Real(real);
}

[[nodiscard]] std::string FormatInteger(std::int64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    std::terminate();
  }
  return {buffer.data(), result.ptr};
}

[[nodiscard]] std::string FormatReal(double value) { return internal::FormatSqliteReal(value); }

[[nodiscard]] std::string FormatNumericValue(const SqlValue& value) {
  if (value.type() == SqlValueType::kInteger) {
    return FormatInteger(SqlValueAccess::Integer(value));
  }
  return FormatReal(SqlValueAccess::Real(value));
}

[[nodiscard]] std::strong_ordering CompareIntegers(std::int64_t left, std::int64_t right) noexcept {
  if (left < right) {
    return std::strong_ordering::less;
  }
  if (left > right) {
    return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering CompareReals(double left, double right) noexcept {
  if (left < right) {
    return std::strong_ordering::less;
  }
  if (left > right) {
    return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering CompareIntegerAndReal(std::int64_t integer,
                                                         double real) noexcept {
  if (real < kInt64LowerBound) {
    return std::strong_ordering::greater;
  }
  if (real >= kInt64UpperBound) {
    return std::strong_ordering::less;
  }

  const auto truncated = static_cast<std::int64_t>(real);
  const std::strong_ordering integer_order = CompareIntegers(integer, truncated);
  if (integer_order != std::strong_ordering::equal) {
    return integer_order;
  }
  return CompareReals(static_cast<double>(integer), real);
}

[[nodiscard]] std::strong_ordering Reverse(std::strong_ordering ordering) noexcept {
  if (ordering == std::strong_ordering::less) {
    return std::strong_ordering::greater;
  }
  if (ordering == std::strong_ordering::greater) {
    return std::strong_ordering::less;
  }
  return std::strong_ordering::equal;
}

[[nodiscard]] int StorageClassRank(SqlValueType type) noexcept {
  switch (type) {
    case SqlValueType::kNull:
      return 0;
    case SqlValueType::kInteger:
    case SqlValueType::kReal:
      return 1;
    case SqlValueType::kText:
      return 2;
    case SqlValueType::kBlob:
      return 3;
  }
  std::terminate();
}

[[nodiscard]] std::strong_ordering CompareBytes(ByteView left, ByteView right) noexcept {
  const std::size_t common_size = std::min(left.size(), right.size());
  if (common_size != 0) {
    const int comparison = std::memcmp(left.data(), right.data(), common_size);
    if (comparison < 0) {
      return std::strong_ordering::less;
    }
    if (comparison > 0) {
      return std::strong_ordering::greater;
    }
  }
  return left.size() <=> right.size();
}

[[nodiscard]] SqlTruthValue ToTruthValue(bool value) noexcept {
  return value ? SqlTruthValue::kTrue : SqlTruthValue::kFalse;
}

}  // namespace

std::strong_ordering CompareSqlIntegers(std::int64_t left, std::int64_t right) noexcept {
  return CompareIntegers(left, right);
}

std::strong_ordering CompareSqlIntegerAndReal(std::int64_t left, double right) noexcept {
  return CompareIntegerAndReal(left, right);
}

std::strong_ordering CompareSqlRealAndInteger(double left, std::int64_t right) noexcept {
  return Reverse(CompareIntegerAndReal(right, left));
}

std::strong_ordering CompareSqlReals(double left, double right) noexcept {
  return CompareReals(left, right);
}

SqlValue SqlValue::Integer(std::int64_t value) noexcept {
  SqlValue result;
  result.storage_.emplace<1>(value);
  return result;
}

SqlValue SqlValue::Real(double value) noexcept {
  if (std::isnan(value)) {
    return {};
  }
  SqlValue result;
  result.storage_.emplace<2>(value);
  return result;
}

SqlValue SqlValue::Text(std::string value) {
  SqlValue result;
  result.storage_.emplace<3>(std::move(value));
  return result;
}

SqlValue SqlValue::Blob(ByteBuffer value) noexcept {
  SqlValue result;
  result.storage_.emplace<4>(std::move(value));
  return result;
}

SqlValue SqlValue::Clone() const {
  switch (type()) {
    case SqlValueType::kNull:
      return {};
    case SqlValueType::kInteger:
      return Integer(SqlValueAccess::Integer(*this));
    case SqlValueType::kReal:
      return Real(SqlValueAccess::Real(*this));
    case SqlValueType::kText:
      return Text(std::string{SqlValueAccess::Text(*this).bytes()});
    case SqlValueType::kBlob:
      return Blob(ByteBuffer::CopyOf(SqlValueAccess::Blob(*this)));
  }
  std::terminate();
}

SqlValueType SqlValue::type() const noexcept { return static_cast<SqlValueType>(storage_.index()); }

std::optional<std::int64_t> SqlValue::integer_value() const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<double> SqlValue::real_value() const noexcept {
  if (const auto* value = std::get_if<double>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<Utf8View> SqlValue::text_value() const noexcept {
  if (const auto* value = std::get_if<std::string>(&storage_); value != nullptr) {
    return Utf8View{*value};
  }
  return std::nullopt;
}

std::optional<ByteView> SqlValue::blob_value() const noexcept {
  if (const auto* value = std::get_if<ByteBuffer>(&storage_); value != nullptr) {
    return value->view();
  }
  return std::nullopt;
}

std::size_t SqlValue::owned_capacity_bytes() const noexcept {
  if (const auto* text = std::get_if<std::string>(&storage_); text != nullptr) {
    return text->capacity();
  }
  if (const auto* blob = std::get_if<ByteBuffer>(&storage_); blob != nullptr) {
    return blob->capacity().value();
  }
  return 0;
}

SqlValue ApplyAffinity(SqlValue value, TypeAffinity affinity) {
  if (affinity == TypeAffinity::kNone || affinity == TypeAffinity::kBlob ||
      value.type() == SqlValueType::kNull || value.type() == SqlValueType::kBlob) {
    return value;
  }

  if (affinity == TypeAffinity::kText) {
    if (value.type() == SqlValueType::kInteger || value.type() == SqlValueType::kReal) {
      return SqlValue::Text(FormatNumericValue(value));
    }
    return value;
  }

  if (value.type() == SqlValueType::kText) {
    auto converted = TryApplyNumericTextAffinity(SqlValueAccess::Text(value).bytes(),
                                                 affinity == TypeAffinity::kReal);
    if (converted.has_value()) {
      return std::move(*converted);
    }
    return value;
  }
  if (value.type() == SqlValueType::kInteger && affinity == TypeAffinity::kReal) {
    return SqlValue::Real(static_cast<double>(SqlValueAccess::Integer(value)));
  }
  if (value.type() == SqlValueType::kReal && affinity != TypeAffinity::kReal) {
    if (const auto integer = AffinityInteger(SqlValueAccess::Real(value)); integer.has_value()) {
      return SqlValue::Integer(*integer);
    }
  }
  return value;
}

SqlValue CastValue(SqlValue value, CastTarget target) {
  if (value.type() == SqlValueType::kNull) {
    return value;
  }

  if (target == CastTarget::kText) {
    if (value.type() == SqlValueType::kText) {
      return value;
    }
    if (value.type() == SqlValueType::kBlob) {
      return SqlValue::Text(std::string{AsStringView(SqlValueAccess::Blob(value))});
    }
    return SqlValue::Text(FormatNumericValue(value));
  }
  if (target == CastTarget::kBlob) {
    if (value.type() == SqlValueType::kBlob) {
      return value;
    }
    const SqlValue text = CastValue(std::move(value), CastTarget::kText);
    return SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(SqlValueAccess::Text(text).bytes())));
  }
  if (target == CastTarget::kInteger) {
    if (value.type() == SqlValueType::kInteger) {
      return value;
    }
    if (value.type() == SqlValueType::kReal) {
      return SqlValue::Integer(TruncateRealToInteger(SqlValueAccess::Real(value)));
    }
    const std::string_view bytes = value.type() == SqlValueType::kText
                                       ? SqlValueAccess::Text(value).bytes()
                                       : AsStringView(SqlValueAccess::Blob(value));
    return SqlValue::Integer(ParseIntegerPrefix(bytes).value);
  }
  if (target == CastTarget::kReal) {
    if (value.type() == SqlValueType::kReal) {
      return value;
    }
    if (value.type() == SqlValueType::kInteger) {
      return SqlValue::Real(static_cast<double>(SqlValueAccess::Integer(value)));
    }
    const std::string_view bytes = value.type() == SqlValueType::kText
                                       ? SqlValueAccess::Text(value).bytes()
                                       : AsStringView(SqlValueAccess::Blob(value));
    const std::string_view numeric_input = BeforeFirstNull(bytes);
    const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
    return SqlValue::Real(prefix.has_digits ? ParseDouble(numeric_input, prefix) : 0.0);
  }

  if (value.type() == SqlValueType::kInteger || value.type() == SqlValueType::kReal) {
    return value;
  }
  const std::string_view bytes = value.type() == SqlValueType::kText
                                     ? SqlValueAccess::Text(value).bytes()
                                     : AsStringView(SqlValueAccess::Blob(value));
  return ParseForcedNumeric(bytes);
}

SqlValue CoerceNumericForArithmetic(const SqlValue& value) {
  if (value.type() == SqlValueType::kNull || value.type() == SqlValueType::kInteger ||
      value.type() == SqlValueType::kReal) {
    return value.Clone();
  }

  const std::string_view bytes = value.type() == SqlValueType::kText
                                     ? SqlValueAccess::Text(value).bytes()
                                     : AsStringView(SqlValueAccess::Blob(value));
  const std::string_view numeric_input = BeforeFirstNull(bytes);
  const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
  if (!prefix.has_digits) {
    return SqlValue::Integer(0);
  }
  if (!prefix.has_real_syntax) {
    const IntegerPrefix integer = ParseIntegerPrefix(bytes);
    if (integer.has_digits && !integer.overflow) {
      return SqlValue::Integer(integer.value);
    }
  }
  return SqlValue::Real(ParseDouble(numeric_input, prefix));
}

std::int64_t CoerceIntegerForBitwise(const SqlValue& value) noexcept {
  if (value.type() == SqlValueType::kInteger) {
    return SqlValueAccess::Integer(value);
  }
  if (value.type() == SqlValueType::kReal) {
    return TruncateRealToInteger(SqlValueAccess::Real(value));
  }
  if (value.type() == SqlValueType::kText) {
    return ParseIntegerPrefix(SqlValueAccess::Text(value).bytes()).value;
  }
  if (value.type() == SqlValueType::kBlob) {
    return ParseIntegerPrefix(AsStringView(SqlValueAccess::Blob(value))).value;
  }
  return 0;
}

SqlTruthValue EvaluateSqlTruth(const SqlValue& value) noexcept {
  if (value.type() == SqlValueType::kNull) {
    return SqlTruthValue::kNull;
  }
  if (value.type() == SqlValueType::kInteger) {
    return SqlValueAccess::Integer(value) == 0 ? SqlTruthValue::kFalse : SqlTruthValue::kTrue;
  }
  if (value.type() == SqlValueType::kReal) {
    return SqlValueAccess::Real(value) == 0.0 ? SqlTruthValue::kFalse : SqlTruthValue::kTrue;
  }

  const std::string_view bytes = value.type() == SqlValueType::kText
                                     ? SqlValueAccess::Text(value).bytes()
                                     : AsStringView(SqlValueAccess::Blob(value));
  const std::string_view numeric_input = BeforeFirstNull(bytes);
  const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
  const double numeric = prefix.has_digits ? ParseDouble(numeric_input, prefix) : 0.0;
  return numeric == 0.0 ? SqlTruthValue::kFalse : SqlTruthValue::kTrue;
}

std::strong_ordering CompareSqlValues(const SqlValue& left, const SqlValue& right) noexcept {
  const int left_rank = StorageClassRank(left.type());
  const int right_rank = StorageClassRank(right.type());
  if (left_rank != right_rank) {
    return left_rank <=> right_rank;
  }

  if (left.type() == SqlValueType::kNull) {
    return std::strong_ordering::equal;
  }
  if (left_rank == 1) {
    if (left.type() == SqlValueType::kInteger && right.type() == SqlValueType::kInteger) {
      return CompareSqlIntegers(SqlValueAccess::Integer(left), SqlValueAccess::Integer(right));
    }
    if (left.type() == SqlValueType::kReal && right.type() == SqlValueType::kReal) {
      return CompareSqlReals(SqlValueAccess::Real(left), SqlValueAccess::Real(right));
    }
    if (left.type() == SqlValueType::kInteger) {
      return CompareSqlIntegerAndReal(SqlValueAccess::Integer(left), SqlValueAccess::Real(right));
    }
    return CompareSqlRealAndInteger(SqlValueAccess::Real(left), SqlValueAccess::Integer(right));
  }
  if (left.type() == SqlValueType::kText) {
    return CompareBytes(AsBytes(SqlValueAccess::Text(left).bytes()),
                        AsBytes(SqlValueAccess::Text(right).bytes()));
  }
  return CompareBytes(SqlValueAccess::Blob(left), SqlValueAccess::Blob(right));
}

SqlTruthValue EvaluateSqlComparison(const SqlValue& left, const SqlValue& right,
                                    SqlComparison comparison) noexcept {
  const bool has_null = left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull;
  if (comparison == SqlComparison::kIs || comparison == SqlComparison::kIsNot) {
    const bool equal = has_null ? left.type() == right.type()
                                : CompareSqlValues(left, right) == std::strong_ordering::equal;
    return ToTruthValue(comparison == SqlComparison::kIs ? equal : !equal);
  }
  if (has_null) {
    return SqlTruthValue::kNull;
  }

  const std::strong_ordering ordering = CompareSqlValues(left, right);
  switch (comparison) {
    case SqlComparison::kEqual:
      return ToTruthValue(ordering == std::strong_ordering::equal);
    case SqlComparison::kNotEqual:
      return ToTruthValue(ordering != std::strong_ordering::equal);
    case SqlComparison::kLess:
      return ToTruthValue(ordering == std::strong_ordering::less);
    case SqlComparison::kLessEqual:
      return ToTruthValue(ordering != std::strong_ordering::greater);
    case SqlComparison::kGreater:
      return ToTruthValue(ordering == std::strong_ordering::greater);
    case SqlComparison::kGreaterEqual:
      return ToTruthValue(ordering != std::strong_ordering::less);
    case SqlComparison::kIs:
    case SqlComparison::kIsNot:
      break;
  }
  std::terminate();
}

}  // namespace modern_sqlite
