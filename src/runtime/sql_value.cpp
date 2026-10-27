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

#include "sql_value_access.hpp"
#include "sqlite_float.hpp"

namespace modern_sqlite {

std::int64_t SqlValueAccess::Integer(const SqlValue& value) noexcept {
  const auto* result = std::get_if<std::int64_t>(&value.value_);
  if (result == nullptr) {
    std::terminate();
  }
  return *result;
}

double SqlValueAccess::Real(const SqlValue& value) noexcept {
  const auto* result = std::get_if<double>(&value.value_);
  if (result == nullptr) {
    std::terminate();
  }
  return *result;
}

Utf8View SqlValueAccess::Text(const SqlValue& value) noexcept {
  const auto result = value.text_value();
  if (!result.has_value()) {
    std::terminate();
  }
  return *result;
}

ByteView SqlValueAccess::Blob(const SqlValue& value) noexcept {
  const auto result = value.blob_value();
  if (!result.has_value()) {
    std::terminate();
  }
  return *result;
}

void SqlValueAccess::SetNull(SqlValue& value) noexcept {
  value.value_.emplace<std::monostate>();
  if (!std::holds_alternative<std::monostate>(value.storage_) &&
      !std::holds_alternative<SqlValue::RetainedBytes>(value.storage_)) {
    value.storage_.emplace<std::monostate>();
  }
}

void SqlValueAccess::SetInteger(SqlValue& value, std::int64_t integer) noexcept {
  SetNull(value);
  value.value_.emplace<std::int64_t>(integer);
}

void SqlValueAccess::SetReal(SqlValue& value, double real) noexcept {
  SetNull(value);
  if (!std::isnan(real)) {
    value.value_.emplace<double>(real);
  }
}

void SqlValueAccess::Release(SqlValue& value) noexcept {
  value.value_.emplace<std::monostate>();
  value.storage_.emplace<std::monostate>();
}

Status SqlValueAccess::CopyText(SqlValue& value, Utf8View source) noexcept {
  return CopyByteValue(value, AsBytes(source.bytes()), SqlValueType::kText);
}

Status SqlValueAccess::CopyBlob(SqlValue& value, ByteView source) noexcept {
  return CopyByteValue(value, source, SqlValueType::kBlob);
}

Result<MutableByteView> SqlValueAccess::PrepareBytes(SqlValue& value, ByteCount size) noexcept {
  SetNull(value);
  auto* retained = std::get_if<SqlValue::RetainedBytes>(&value.storage_);
  if (retained == nullptr) {
    retained = &value.storage_.emplace<SqlValue::RetainedBytes>();
  }
  return RetainStorage(retained->buffer, size, kTerminatorBytes, std::nullopt);
}

void SqlValueAccess::CommitBytes(SqlValue& value, ByteCount size, SqlValueType type) noexcept {
  const auto* retained = std::get_if<SqlValue::RetainedBytes>(&value.storage_);
  if (retained == nullptr || retained->buffer.size().value() < size.value() ||
      retained->buffer.size().value() - size.value() < kTerminatorBytes.value()) {
    std::terminate();
  }
  const ByteView bytes = retained->buffer.view().first(size.value());
  if (type == SqlValueType::kText) {
    value.value_.emplace<Utf8View>(Utf8View{AsStringView(bytes)});
  } else if (type == SqlValueType::kBlob) {
    value.value_.emplace<ByteView>(bytes);
  } else {
    std::terminate();
  }
}

void SqlValueAccess::ShareColumnBytes(SqlValue& value, std::shared_ptr<const ByteBuffer> storage,
                                      ByteCount size, SqlValueType type) noexcept {
  if (storage == nullptr || size.value() > storage->size().value()) {
    std::terminate();
  }
  if (storage->size().value() - size.value() < kSharedColumnTerminatorBytes.value()) {
    std::terminate();
  }
  const ByteView bytes = storage->view().first(size.value());
  Release(value);
  value.storage_.emplace<SqlValue::SharedBytes>(
      SqlValue::SharedBytes{.buffer = std::move(storage)});
  if (type == SqlValueType::kText) {
    value.value_.emplace<Utf8View>(Utf8View{AsStringView(bytes)});
  } else if (type == SqlValueType::kBlob) {
    value.value_.emplace<ByteView>(bytes);
  } else {
    std::terminate();
  }
}

bool SqlValueAccess::IsStaticBorrow(const SqlValue& value) noexcept {
  return std::holds_alternative<SqlValue::BorrowedText>(value.value_) ||
         std::holds_alternative<SqlValue::BorrowedBlob>(value.value_);
}

void SqlValueAccess::BorrowStable(SqlValue& destination, const SqlValue& source) noexcept {
  if (&destination == &source) {
    return;
  }
  switch (source.type()) {
    case SqlValueType::kNull:
      SetNull(destination);
      return;
    case SqlValueType::kInteger:
      SetInteger(destination, Integer(source));
      return;
    case SqlValueType::kReal:
      SetReal(destination, Real(source));
      return;
    case SqlValueType::kText: {
      const Utf8View text = Text(source);
      SetNull(destination);
      destination.value_.emplace<SqlValue::BorrowedText>(SqlValue::BorrowedText{.view = text});
      return;
    }
    case SqlValueType::kBlob: {
      BorrowStableBlob(destination, Blob(source));
      return;
    }
  }
  std::terminate();
}

void SqlValueAccess::BorrowStableBlob(SqlValue& destination, ByteView source) noexcept {
  SetNull(destination);
  destination.value_.emplace<SqlValue::BorrowedBlob>(SqlValue::BorrowedBlob{.view = source});
}

Status SqlValueAccess::Copy(SqlValue& destination, const SqlValue& source) noexcept {
  if (&destination == &source) {
    return {};
  }
  if (IsStaticBorrow(source)) {
    BorrowStable(destination, source);
    return {};
  }
  switch (source.type()) {
    case SqlValueType::kNull:
      SetNull(destination);
      return {};
    case SqlValueType::kInteger:
      SetInteger(destination, Integer(source));
      return {};
    case SqlValueType::kReal:
      SetReal(destination, Real(source));
      return {};
    case SqlValueType::kText:
      return CopyText(destination, Text(source));
    case SqlValueType::kBlob:
      return CopyBlob(destination, Blob(source));
  }
  std::terminate();
}

Status SqlValueAccess::CopyByteValue(SqlValue& value, ByteView source, SqlValueType type) noexcept {
  ByteBuffer replacement;
  auto* retained = std::get_if<SqlValue::RetainedBytes>(&value.storage_);
  ByteBuffer& buffer = retained != nullptr ? retained->buffer : replacement;
  auto copied = RetainBytes(buffer, source, kTerminatorBytes);
  if (!copied.has_value()) {
    return std::unexpected(std::move(copied.error()));
  }
  value.value_.emplace<std::monostate>();
  if (retained == nullptr) {
    value.storage_.emplace<SqlValue::RetainedBytes>(
        SqlValue::RetainedBytes{std::move(replacement)});
  }
  if (type == SqlValueType::kText) {
    value.value_.emplace<Utf8View>(Utf8View{AsStringView(*copied)});
  } else {
    value.value_.emplace<ByteView>(*copied);
  }
  return {};
}

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

enum class NumericTextConversion : std::uint8_t {
  kNumericAffinity,
  kRealAffinity,
  kFunctionNumericType,
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

[[nodiscard]] double ParseRealPrefix(std::string_view bytes) noexcept {
  const std::string_view numeric_input = BeforeFirstNull(bytes);
  const DecimalPrefix prefix = ScanDecimalPrefix(numeric_input);
  return prefix.has_digits ? ParseDouble(numeric_input, prefix) : 0.0;
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

[[nodiscard]] std::optional<SqlValue> TryApplyNumericTextAffinity(
    std::string_view bytes, NumericTextConversion conversion) {
  const bool force_real = conversion == NumericTextConversion::kRealAffinity;
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
  if (conversion == NumericTextConversion::kFunctionNumericType) {
    const std::int64_t exact_integer = TruncateRealToInteger(real);
    if (!prefix.has_real_syntax && RealSameAsInteger(real, exact_integer)) {
      return SqlValue::Integer(exact_integer);
    }
  } else if (const auto exact_integer = AffinityInteger(real); exact_integer.has_value()) {
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

SqlValue::SqlValue(SqlValue&& other) noexcept
    : storage_(std::move(other.storage_)), value_(other.value_) {
  RebindOwnedText();
  SqlValueAccess::Release(other);
}

SqlValue& SqlValue::operator=(SqlValue&& other) noexcept {
  if (this != &other) {
    SqlValueAccess::Release(*this);
    storage_ = std::move(other.storage_);
    value_ = other.value_;
    RebindOwnedText();
    SqlValueAccess::Release(other);
  }
  return *this;
}

void SqlValue::RebindOwnedText() noexcept {
  if (const auto* text = std::get_if<std::string>(&storage_); text != nullptr) {
    value_.emplace<Utf8View>(Utf8View{*text});
  }
}

SqlValue SqlValue::Integer(std::int64_t value) noexcept {
  SqlValue result;
  result.value_.emplace<std::int64_t>(value);
  return result;
}

SqlValue SqlValue::Real(double value) noexcept {
  if (std::isnan(value)) {
    return {};
  }
  SqlValue result;
  result.value_.emplace<double>(value);
  return result;
}

SqlValue SqlValue::Text(std::string value) {
  SqlValue result;
  const auto& text = result.storage_.emplace<std::string>(std::move(value));
  result.value_.emplace<Utf8View>(Utf8View{text});
  return result;
}

SqlValue SqlValue::Blob(ByteBuffer value) noexcept {
  SqlValue result;
  const auto& blob = result.storage_.emplace<ByteBuffer>(std::move(value));
  result.value_.emplace<ByteView>(blob.view());
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

SqlValueType SqlValue::type() const noexcept {
  static constexpr std::array kLogicalTypes{
      SqlValueType::kNull, SqlValueType::kInteger, SqlValueType::kReal, SqlValueType::kText,
      SqlValueType::kBlob, SqlValueType::kText,    SqlValueType::kBlob,
  };
  static_assert(kLogicalTypes.size() == std::variant_size_v<LogicalValue>);
  return kLogicalTypes[value_.index()];
}

std::optional<std::int64_t> SqlValue::integer_value() const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&value_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<double> SqlValue::real_value() const noexcept {
  if (const auto* value = std::get_if<double>(&value_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<Utf8View> SqlValue::text_value() const noexcept {
  if (const auto* value = std::get_if<Utf8View>(&value_); value != nullptr) {
    return *value;
  }
  if (const auto* value = std::get_if<BorrowedText>(&value_); value != nullptr) {
    return value->view;
  }
  return std::nullopt;
}

std::optional<ByteView> SqlValue::blob_value() const noexcept {
  if (const auto* value = std::get_if<ByteView>(&value_); value != nullptr) {
    return *value;
  }
  if (const auto* value = std::get_if<BorrowedBlob>(&value_); value != nullptr) {
    return value->view;
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
  if (const auto* retained = std::get_if<RetainedBytes>(&storage_); retained != nullptr) {
    return retained->buffer.capacity().value();
  }
  if (const auto* shared = std::get_if<SharedBytes>(&storage_); shared != nullptr) {
    return shared->buffer->capacity().value();
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
                                                 affinity == TypeAffinity::kReal
                                                     ? NumericTextConversion::kRealAffinity
                                                     : NumericTextConversion::kNumericAffinity);
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

SqlValue ApplyNumericType(SqlValue value) {
  if (value.type() != SqlValueType::kText) {
    return value;
  }
  auto converted = TryApplyNumericTextAffinity(SqlValueAccess::Text(value).bytes(),
                                               NumericTextConversion::kFunctionNumericType);
  if (converted.has_value()) {
    return std::move(*converted);
  }
  return value;
}

std::optional<SqlValue> SqlValueAccess::ConvertNumericOperand(const SqlValue& value,
                                                              NumericOperandPolicy policy) {
  switch (policy) {
    case NumericOperandPolicy::Arithmetic:
      return CoerceNumericForArithmetic(value);
    case NumericOperandPolicy::Function:
      if (value.type() == SqlValueType::kText) {
        return TryApplyNumericTextAffinity(Text(value).bytes(),
                                           NumericTextConversion::kFunctionNumericType);
      }
      return std::nullopt;
  }
  std::terminate();
}

double SqlValueAccess::RealNumber(const SqlValue& value) noexcept {
  switch (value.type()) {
    case SqlValueType::kNull:
      return 0.0;
    case SqlValueType::kInteger:
      return static_cast<double>(Integer(value));
    case SqlValueType::kReal:
      return Real(value);
    case SqlValueType::kText:
      return ParseRealPrefix(Text(value).bytes());
    case SqlValueType::kBlob:
      return ParseRealPrefix(AsStringView(Blob(value)));
  }
  std::terminate();
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
    return SqlValue::Real(ParseRealPrefix(bytes));
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

std::uint64_t HashSqlValueForMembership(const SqlValue& value) noexcept {
  switch (value.type()) {
    case SqlValueType::kNull:
      return 0;
    case SqlValueType::kInteger:
    case SqlValueType::kReal:
      return static_cast<std::uint64_t>(CoerceIntegerForBitwise(value));
    case SqlValueType::kText:
    case SqlValueType::kBlob:
      break;
    default:
      std::terminate();
  }
  const bool text = value.type() == SqlValueType::kText;
  const ByteView bytes =
      text ? AsBytes(SqlValueAccess::Text(value).bytes()) : SqlValueAccess::Blob(value);
  auto hash = static_cast<std::uint64_t>(bytes.size());
  std::uint64_t word = 0;
  const std::size_t prefix_size = std::min(bytes.size(), sizeof(word));
  if (prefix_size != 0U) {
    std::memcpy(&word, bytes.data(), prefix_size);
    hash += word;
  }
  if (text && bytes.size() >= sizeof(word)) {
    std::memcpy(&word, bytes.last(sizeof(word)).data(), sizeof(word));
    hash += word;
  }
  return hash;
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
  const double numeric = ParseRealPrefix(bytes);
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
