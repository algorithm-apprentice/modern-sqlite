#include "modern_sqlite/runtime/function_registry.hpp"

#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/text/text.hpp"

#ifndef MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE
#define MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE 0
#endif

#if MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE != 0 && MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE != 1
#error "MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE must be 0 or 1"
#endif

namespace modern_sqlite {
namespace {

[[nodiscard]] std::string FunctionErrorMessage(std::string_view prefix, std::string_view name,
                                               std::string_view suffix = {}) {
  std::string message;
  message.reserve(prefix.size() + name.size() + suffix.size());
  message.append(prefix);
  message.append(name);
  message.append(suffix);
  return message;
}

[[nodiscard]] bool FunctionNamesEqual(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(left[index]));
    const auto right_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(right[index]));
    if (SqliteToLower(left_byte) != SqliteToLower(right_byte)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::int64_t RequiredInteger(const SqlValue& value) noexcept {
  const std::optional<std::int64_t> integer = value.integer_value();
  if (!integer.has_value()) {
    std::terminate();
  }
  return *integer;
}

[[nodiscard]] double RequiredReal(const SqlValue& value) noexcept {
  const std::optional<double> real = value.real_value();
  if (!real.has_value()) {
    std::terminate();
  }
  return *real;
}

[[nodiscard]] std::string_view RequiredText(const SqlValue& value) noexcept {
  const std::optional<Utf8View> text = value.text_value();
  if (!text.has_value()) {
    std::terminate();
  }
  return text->bytes();
}

[[nodiscard]] ByteView RequiredBlob(const SqlValue& value) noexcept {
  const std::optional<ByteView> blob = value.blob_value();
  if (!blob.has_value()) {
    std::terminate();
  }
  return *blob;
}

[[nodiscard]] Result<SqlValue> LengthResult(std::size_t length) {
  constexpr auto kMaximumLength =
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
  if (length > kMaximumLength) {
    return std::unexpected(Error::Create(ErrorCode::kTooLarge, "string or blob too big"));
  }
  return SqlValue::Integer(static_cast<std::int64_t>(length));
}

[[nodiscard]] std::size_t CountSqliteUtf8Characters(std::string_view text) noexcept {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < text.size()) {
    const auto first = static_cast<std::uint8_t>(static_cast<unsigned char>(text[offset]));
    if (first == 0U) {
      break;
    }
    ++offset;
    ++count;
    if (first >= 0x80U) {
      while (offset < text.size()) {
        const auto continuation =
            static_cast<std::uint8_t>(static_cast<unsigned char>(text[offset]));
        if ((continuation & 0xc0U) != 0x80U) {
          break;
        }
        ++offset;
      }
    }
  }
  return count;
}

[[nodiscard]] std::string TextRepresentation(const SqlValue& value) {
  switch (value.type()) {
    case SqlValueType::kNull:
      return {};
    case SqlValueType::kInteger:
    case SqlValueType::kReal: {
      const SqlValue text = CastValue(value.Clone(), CastTarget::kText);
      return std::string{RequiredText(text)};
    }
    case SqlValueType::kText:
      return std::string{RequiredText(value)};
    case SqlValueType::kBlob:
      return std::string{AsStringView(RequiredBlob(value))};
  }
  std::terminate();
}

[[nodiscard]] Result<SqlValue> TypeofFunction(const ScalarFunctionContext&,
                                              std::span<const SqlValue> arguments) {
  std::string_view type_name;
  switch (arguments.front().type()) {
    case SqlValueType::kNull:
      type_name = "null";
      break;
    case SqlValueType::kInteger:
      type_name = "integer";
      break;
    case SqlValueType::kReal:
      type_name = "real";
      break;
    case SqlValueType::kText:
      type_name = "text";
      break;
    case SqlValueType::kBlob:
      type_name = "blob";
      break;
  }
  return SqlValue::Text(std::string{type_name});
}

[[nodiscard]] Result<SqlValue> LengthFunction(const ScalarFunctionContext&,
                                              std::span<const SqlValue> arguments) {
  const SqlValue& value = arguments.front();
  switch (value.type()) {
    case SqlValueType::kNull:
      return SqlValue{};
    case SqlValueType::kBlob:
      return LengthResult(RequiredBlob(value).size());
    case SqlValueType::kText:
      return LengthResult(CountSqliteUtf8Characters(RequiredText(value)));
    case SqlValueType::kInteger:
    case SqlValueType::kReal: {
      const SqlValue text = CastValue(value.Clone(), CastTarget::kText);
      return LengthResult(RequiredText(text).size());
    }
  }
  std::terminate();
}

[[nodiscard]] Result<SqlValue> AbsFunction(const ScalarFunctionContext&,
                                           std::span<const SqlValue> arguments) {
  const SqlValue& value = arguments.front();
  if (value.type() == SqlValueType::kNull) {
    return SqlValue{};
  }
  if (value.type() == SqlValueType::kInteger) {
    const std::int64_t integer = RequiredInteger(value);
    if (integer == std::numeric_limits<std::int64_t>::min()) {
      return std::unexpected(Error::Create(ErrorCode::kGeneric, "integer overflow"));
    }
    return SqlValue::Integer(integer < 0 ? -integer : integer);
  }

  const SqlValue real_value = value.type() == SqlValueType::kReal
                                  ? value.Clone()
                                  : CastValue(value.Clone(), CastTarget::kReal);
  const double real = RequiredReal(real_value);
  return SqlValue::Real(real < 0.0 ? -real : real);
}

[[nodiscard]] Result<SqlValue> ConvertAsciiCase(std::span<const SqlValue> arguments,
                                                bool uppercase) {
  if (arguments.front().type() == SqlValueType::kNull) {
    return SqlValue{};
  }

  std::string text = TextRepresentation(arguments.front());
  for (char& character : text) {
    const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    character = static_cast<char>(uppercase ? SqliteToUpper(byte) : SqliteToLower(byte));
  }
  return SqlValue::Text(std::move(text));
}

[[nodiscard]] Result<SqlValue> LowerFunction(const ScalarFunctionContext&,
                                             std::span<const SqlValue> arguments) {
  return ConvertAsciiCase(arguments, false);
}

[[nodiscard]] Result<SqlValue> UpperFunction(const ScalarFunctionContext&,
                                             std::span<const SqlValue> arguments) {
  return ConvertAsciiCase(arguments, true);
}

[[nodiscard]] Result<SqlValue> SignFunction(const ScalarFunctionContext&,
                                            std::span<const SqlValue> arguments) {
  const SqlValue numeric = ApplyAffinity(arguments.front().Clone(), TypeAffinity::kNumeric);
  if (numeric.type() == SqlValueType::kInteger) {
    const std::int64_t integer = RequiredInteger(numeric);
    return SqlValue::Integer(integer < 0 ? -1 : integer > 0 ? 1 : 0);
  }
  if (numeric.type() == SqlValueType::kReal) {
    const double real = RequiredReal(numeric);
    return SqlValue::Integer(real < 0.0 ? -1 : real > 0.0 ? 1 : 0);
  }
  return SqlValue{};
}

[[nodiscard]] Result<SqlValue> NullifFunction(const ScalarFunctionContext& context,
                                              std::span<const SqlValue> arguments) {
  if (CompareSqlValues(arguments[0], arguments[1], context.collation()) ==
      std::weak_ordering::equivalent) {
    return SqlValue{};
  }
  return arguments[0].Clone();
}

[[nodiscard]] Result<SqlValue> MinMaxFunction(const ScalarFunctionContext& context,
                                              std::span<const SqlValue> arguments,
                                              bool select_minimum) {
  std::size_t best = 0;
  if (arguments.front().type() == SqlValueType::kNull) {
    return SqlValue{};
  }
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index].type() == SqlValueType::kNull) {
      return SqlValue{};
    }
    const std::weak_ordering ordering =
        CompareSqlValues(arguments[best], arguments[index], context.collation());
    const bool select_candidate = select_minimum ? ordering != std::weak_ordering::less
                                                 : ordering == std::weak_ordering::less;
    if (select_candidate) {
      best = index;
    }
  }
  return arguments[best].Clone();
}

[[nodiscard]] Result<SqlValue> MinFunction(const ScalarFunctionContext& context,
                                           std::span<const SqlValue> arguments) {
  return MinMaxFunction(context, arguments, true);
}

[[nodiscard]] Result<SqlValue> MaxFunction(const ScalarFunctionContext& context,
                                           std::span<const SqlValue> arguments) {
  return MinMaxFunction(context, arguments, false);
}

#if MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE
[[nodiscard]] Result<SqlValue> InstrumentationProbe(const ScalarFunctionContext&,
                                                    std::span<const SqlValue> arguments) {
  const std::optional<std::int64_t> tag = arguments[0].integer_value();
  if (!tag.has_value() || *tag <= 0 ||
      static_cast<std::uint64_t>(*tag) >= instrumentation::kProbeTagCount) {
    return std::unexpected(Error::Create(ErrorCode::kMisuse, "modern_sqlite_probe tag is invalid"));
  }
  MODERN_SQLITE_RECORD_PROBE_CALL(static_cast<std::size_t>(*tag));
  return arguments[1].Clone();
}
#endif

constexpr std::array kCoreFunctions{
    ScalarFunction{"typeof", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, TypeofFunction},
    ScalarFunction{"length", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, LengthFunction},
    ScalarFunction{"abs", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, AbsFunction},
    ScalarFunction{"lower", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, LowerFunction},
    ScalarFunction{"upper", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, UpperFunction},
    ScalarFunction{"sign", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kNone, SignFunction},
    ScalarFunction{"nullif", FunctionArity::Exact(2), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kRequired, NullifFunction},
    ScalarFunction{"min", FunctionArity::AtLeast(2), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kRequired, MinFunction},
    ScalarFunction{"max", FunctionArity::AtLeast(2), FunctionDeterminism::kDeterministic,
                   FunctionCollationUse::kRequired, MaxFunction},
#if MODERN_SQLITE_ENABLE_PERFORMANCE_PROBE
    ScalarFunction{"modern_sqlite_probe", FunctionArity::Exact(2),
                   FunctionDeterminism::kNonDeterministic, FunctionCollationUse::kNone,
                   InstrumentationProbe},
#endif
};

constexpr FunctionRegistry kCoreRegistry{
    std::span<const ScalarFunction>{kCoreFunctions},
};

}  // namespace

Result<SqlValue> ScalarFunction::Invoke(const ScalarFunctionContext& context,
                                        std::span<const SqlValue> arguments) const {
  if (!arity_.Accepts(arguments.size())) {
    return std::unexpected(
        Error::Create(ErrorCode::kGeneric,
                      FunctionErrorMessage("wrong number of arguments to function ", name_, "()")));
  }
  if (callback_ == nullptr) {
    return std::unexpected(
        Error::Create(ErrorCode::kInternal,
                      FunctionErrorMessage("function has no scalar implementation: ", name_)));
  }
  return callback_(context, arguments);
}

Result<const ScalarFunction*> FunctionRegistry::Resolve(std::string_view name,
                                                        std::size_t argument_count) const {
  const ScalarFunction* best = nullptr;
  bool name_found = false;
  for (const ScalarFunction& function : functions_) {
    if (!FunctionNamesEqual(function.name(), name)) {
      continue;
    }
    name_found = true;
    if (!function.arity().Accepts(argument_count)) {
      continue;
    }
    if (best == nullptr || (!best->arity().is_exact() && function.arity().is_exact())) {
      best = &function;
    }
  }

  if (best != nullptr) {
    return best;
  }
  if (name_found) {
    return std::unexpected(
        Error::Create(ErrorCode::kGeneric,
                      FunctionErrorMessage("wrong number of arguments to function ", name, "()")));
  }
  return std::unexpected(
      Error::Create(ErrorCode::kGeneric, FunctionErrorMessage("no such function: ", name)));
}

const FunctionRegistry& CoreFunctionRegistry() noexcept { return kCoreRegistry; }

}  // namespace modern_sqlite
