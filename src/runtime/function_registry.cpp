#include "modern_sqlite/runtime/function_registry.hpp"

#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/text/text.hpp"
#include "sql_value_access.hpp"

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

struct AggregateTextRepresentation {
  const SqlValue* borrowed = nullptr;
  std::optional<std::string> owned;

  [[nodiscard]] std::string_view bytes() const noexcept {
    if (owned.has_value()) {
      return *owned;
    }
    if (borrowed == nullptr || borrowed->type() == SqlValueType::kNull) {
      return {};
    }
    if (borrowed->type() == SqlValueType::kText) {
      return RequiredText(*borrowed);
    }
    if (borrowed->type() == SqlValueType::kBlob) {
      return AsStringView(RequiredBlob(*borrowed));
    }
    std::terminate();
  }
};

[[nodiscard]] AggregateTextRepresentation AggregateText(const SqlValue& value) {
  if (value.type() == SqlValueType::kText || value.type() == SqlValueType::kBlob ||
      value.type() == SqlValueType::kNull) {
    return AggregateTextRepresentation{
        .borrowed = &value,
        .owned = std::nullopt,
    };
  }
  return AggregateTextRepresentation{
      .borrowed = nullptr,
      .owned = TextRepresentation(value),
  };
}

struct CountAggregateState {
  std::int64_t count = 0;
};

struct NumericAggregateState {
  double real_sum = 0.0;
  double error = 0.0;
  std::int64_t integer_sum = 0;
  std::int64_t count = 0;
  bool approximate = false;
  bool overflow = false;
};

struct MinMaxAggregateState {
  std::optional<SqlValue> best;
};

struct GroupConcatAggregateState {
  std::string text;
  std::optional<Error> error;
  bool has_value = false;
};

using AggregatePayload = std::variant<std::monostate, CountAggregateState, NumericAggregateState,
                                      MinMaxAggregateState, GroupConcatAggregateState>;

[[nodiscard]] bool AddInt64(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return false;
  }
  result = left + right;
  return true;
}

void KahanBabuskaNeumaierStep(NumericAggregateState& state, double value) noexcept {
  const double sum = state.real_sum;
  const double next = sum + value;
  if (std::fabs(sum) > std::fabs(value)) {
    state.error += (sum - next) + value;
  } else {
    state.error += (value - next) + sum;
  }
  state.real_sum = next;
}

void KahanBabuskaNeumaierStepInt64(NumericAggregateState& state, std::int64_t value) noexcept {
  constexpr std::int64_t kExactDoubleLimit = INT64_C(4503599627370496);
  if (value <= -kExactDoubleLimit || value >= kExactDoubleLimit) {
    const std::int64_t small = value % INT64_C(16384);
    const std::int64_t large = value - small;
    KahanBabuskaNeumaierStep(state, static_cast<double>(large));
    KahanBabuskaNeumaierStep(state, static_cast<double>(small));
    return;
  }
  KahanBabuskaNeumaierStep(state, static_cast<double>(value));
}

void InitializeKahanBabuskaNeumaier(NumericAggregateState& state, std::int64_t value) noexcept {
  constexpr std::int64_t kExactDoubleLimit = INT64_C(4503599627370496);
  if (value <= -kExactDoubleLimit || value >= kExactDoubleLimit) {
    const std::int64_t small = value % INT64_C(16384);
    state.real_sum = static_cast<double>(value - small);
    state.error = static_cast<double>(small);
    return;
  }
  state.real_sum = static_cast<double>(value);
  state.error = 0.0;
}

[[nodiscard]] Result<AggregateStepEffect> StepCount(CountAggregateState& state,
                                                    std::span<const SqlValue> arguments) {
  if (!arguments.empty() && arguments.front().type() == SqlValueType::kNull) {
    return AggregateStepEffect::kNoRepresentativePreference;
  }
  if (state.count == std::numeric_limits<std::int64_t>::max()) {
    return std::unexpected(Error::Create(ErrorCode::kTooLarge, "count overflow"));
  }
  ++state.count;
  return AggregateStepEffect::kNoRepresentativePreference;
}

[[nodiscard]] Result<AggregateStepEffect> StepNumeric(NumericAggregateState& state,
                                                      std::span<const SqlValue> arguments) {
  std::optional<SqlValue> converted;
  const SqlValue& value = SqlValueAccess::NumericOperand(
      arguments.front(), converted, SqlValueAccess::NumericOperandPolicy::Function);
  if (value.type() == SqlValueType::kNull) {
    return AggregateStepEffect::kNoRepresentativePreference;
  }
  if (state.count == std::numeric_limits<std::int64_t>::max()) {
    return std::unexpected(Error::Create(ErrorCode::kTooLarge, "aggregate count overflow"));
  }
  ++state.count;

  if (!state.approximate) {
    if (value.type() == SqlValueType::kInteger) {
      const std::int64_t integer = RequiredInteger(value);
      std::int64_t sum = 0;
      if (AddInt64(state.integer_sum, integer, sum)) {
        state.integer_sum = sum;
        return AggregateStepEffect::kNoRepresentativePreference;
      }
      state.overflow = true;
      InitializeKahanBabuskaNeumaier(state, state.integer_sum);
      state.approximate = true;
      KahanBabuskaNeumaierStepInt64(state, integer);
      return AggregateStepEffect::kNoRepresentativePreference;
    }
    InitializeKahanBabuskaNeumaier(state, state.integer_sum);
    state.approximate = true;
    KahanBabuskaNeumaierStep(state, SqlValueAccess::RealNumber(value));
    return AggregateStepEffect::kNoRepresentativePreference;
  }

  if (value.type() == SqlValueType::kInteger) {
    KahanBabuskaNeumaierStepInt64(state, RequiredInteger(value));
  } else {
    state.overflow = false;
    KahanBabuskaNeumaierStep(state, SqlValueAccess::RealNumber(value));
  }
  return AggregateStepEffect::kNoRepresentativePreference;
}

[[nodiscard]] Result<AggregateStepEffect> StepMinMax(MinMaxAggregateState& state,
                                                     const AggregateFunctionContext& context,
                                                     const SqlValue& value, bool select_minimum) {
  if (value.type() == SqlValueType::kNull) {
    return state.best.has_value() ? AggregateStepEffect::kKeepRepresentative
                                  : AggregateStepEffect::kReplaceRepresentative;
  }
  if (!state.best.has_value()) {
    state.best.emplace(value.Clone());
    return AggregateStepEffect::kReplaceRepresentative;
  }
  const std::weak_ordering ordering = CompareSqlValues(*state.best, value, context.collation());
  const bool replace = select_minimum ? ordering == std::weak_ordering::greater
                                      : ordering == std::weak_ordering::less;
  if (!replace) {
    return AggregateStepEffect::kKeepRepresentative;
  }
  SqlValue replacement = value.Clone();
  state.best = std::move(replacement);
  return AggregateStepEffect::kReplaceRepresentative;
}

void LatchAggregateError(GroupConcatAggregateState& state, Error error) noexcept {
  if (!state.error.has_value()) {
    state.error.emplace(std::move(error));
  }
}

void LatchTooLarge(GroupConcatAggregateState& state) noexcept {
  if (state.error.has_value()) {
    return;
  }
  try {
    state.error.emplace(Error::Create(ErrorCode::kTooLarge, "string or blob too big"));
  } catch (const std::bad_alloc&) {
    state.error.emplace(Error::OutOfMemory());
  } catch (const std::length_error&) {
    state.error.emplace(Error::OutOfMemory());
  }
}

void AppendAggregateText(GroupConcatAggregateState& state, std::string_view text,
                         ByteCount maximum_value_bytes) noexcept {
  if (state.error.has_value()) {
    return;
  }
  const std::size_t maximum = maximum_value_bytes.value();
  if (text.size() > maximum || state.text.size() > maximum - text.size()) {
    LatchTooLarge(state);
    return;
  }
  try {
    state.text.append(text);
  } catch (const std::bad_alloc&) {
    LatchAggregateError(state, Error::OutOfMemory());
  } catch (const std::length_error&) {
    LatchAggregateError(state, Error::OutOfMemory());
  }
}

[[nodiscard]] Result<AggregateStepEffect> StepGroupConcat(GroupConcatAggregateState& state,
                                                          const AggregateFunctionContext& context,
                                                          std::span<const SqlValue> arguments) {
  if (arguments.front().type() == SqlValueType::kNull) {
    return AggregateStepEffect::kNoRepresentativePreference;
  }

  const bool first = !state.has_value;
  if (arguments.size() == 1U) {
    if (!first) {
      AppendAggregateText(state, ",", context.maximum_value_bytes());
    }
  } else {
    const AggregateTextRepresentation separator = AggregateText(arguments[1]);
    if (!first) {
      AppendAggregateText(state, separator.bytes(), context.maximum_value_bytes());
    }
  }

  const AggregateTextRepresentation value = AggregateText(arguments.front());
  AppendAggregateText(state, value.bytes(), context.maximum_value_bytes());
  state.has_value = true;
  return AggregateStepEffect::kNoRepresentativePreference;
}

[[nodiscard]] double NumericAggregateResult(const NumericAggregateState& state) noexcept {
  if (!state.approximate) {
    return static_cast<double>(state.integer_sum);
  }
  return std::isfinite(state.error) ? state.real_sum + state.error : state.real_sum;
}

[[nodiscard]] Result<SqlValue> TypeofResult(SqlValueType type) {
  std::string_view type_name;
  switch (type) {
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

[[nodiscard]] Result<SqlValue> TypeofFunction(const ScalarFunctionContext&,
                                              std::span<const SqlValue> arguments) {
  return TypeofResult(arguments.front().type());
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

  const double real = SqlValueAccess::RealNumber(value);
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
  std::optional<SqlValue> converted;
  const SqlValue& numeric = SqlValueAccess::NumericOperand(
      arguments.front(), converted, SqlValueAccess::NumericOperandPolicy::Function);
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

}  // namespace

struct AggregateState::Impl {
  AggregatePayload payload;
};

AggregateState::AggregateState(const AggregateFunction& function) noexcept : function_(&function) {}

AggregateState::AggregateState(AggregateState&& other) noexcept
    : function_(std::exchange(other.function_, nullptr)),
      impl_(std::move(other.impl_)),
      finalized_(std::exchange(other.finalized_, false)) {}

AggregateState& AggregateState::operator=(AggregateState&& other) noexcept {
  if (this != &other) {
    function_ = std::exchange(other.function_, nullptr);
    impl_ = std::move(other.impl_);
    finalized_ = std::exchange(other.finalized_, false);
  }
  return *this;
}

AggregateState::~AggregateState() = default;

bool AggregateState::valid() const noexcept { return function_ != nullptr; }

bool AggregateState::initialized() const noexcept { return valid() && impl_ != nullptr; }

bool AggregateState::finalized() const noexcept { return valid() && finalized_; }

void AggregateState::Reset() noexcept {
  impl_.reset();
  finalized_ = false;
}

AggregateState AggregateFunction::CreateState() const noexcept { return AggregateState{*this}; }

Result<AggregateStepEffect> AggregateFunction::Step(AggregateState& state,
                                                    const AggregateFunctionContext& context,
                                                    std::span<const SqlValue> arguments) const {
  try {
    if (!state.valid()) {
      return std::unexpected(Error::Create(ErrorCode::kMisuse, "aggregate state is invalid"));
    }
    if (state.function_ != this) {
      return std::unexpected(
          Error::Create(ErrorCode::kMisuse, "aggregate state belongs to another function"));
    }
    if (state.finalized_) {
      return std::unexpected(Error::Create(ErrorCode::kMisuse, "aggregate state is finalized"));
    }
    if (!arity_.Accepts(arguments.size())) {
      return std::unexpected(Error::Create(
          ErrorCode::kGeneric,
          FunctionErrorMessage("wrong number of arguments to function ", name_, "()")));
    }

    if (kind_ == Kind::kGroupConcat && arguments.front().type() == SqlValueType::kNull) {
      return AggregateStepEffect::kNoRepresentativePreference;
    }

    const auto ensure_state = [&state]<typename State>() -> Result<State*> {
      if (state.impl_ == nullptr) {
        auto impl = std::make_unique<AggregateState::Impl>();
        impl->payload.template emplace<State>();
        state.impl_ = std::move(impl);
      }
      State* const payload = std::get_if<State>(&state.impl_->payload);
      if (payload == nullptr) {
        return std::unexpected(
            Error::Create(ErrorCode::kInternal, "aggregate state payload is invalid"));
      }
      return payload;
    };

    switch (kind_) {
      case Kind::kCount: {
        auto payload = ensure_state.template operator()<CountAggregateState>();
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        return StepCount(**payload, arguments);
      }
      case Kind::kSum:
      case Kind::kTotal:
      case Kind::kAverage: {
        auto payload = ensure_state.template operator()<NumericAggregateState>();
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        return StepNumeric(**payload, arguments);
      }
      case Kind::kMinimum:
      case Kind::kMaximum: {
        auto payload = ensure_state.template operator()<MinMaxAggregateState>();
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        return StepMinMax(**payload, context, arguments.front(), kind_ == Kind::kMinimum);
      }
      case Kind::kGroupConcat: {
        auto payload = ensure_state.template operator()<GroupConcatAggregateState>();
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        return StepGroupConcat(**payload, context, arguments);
      }
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
  return std::unexpected(Error::Create(ErrorCode::kInternal, "aggregate function kind is invalid"));
}

Result<SqlValue> AggregateFunction::Finalize(AggregateState& state) const {
  try {
    if (!state.valid()) {
      return std::unexpected(Error::Create(ErrorCode::kMisuse, "aggregate state is invalid"));
    }
    if (state.function_ != this) {
      return std::unexpected(
          Error::Create(ErrorCode::kMisuse, "aggregate state belongs to another function"));
    }
    if (state.finalized_) {
      return std::unexpected(Error::Create(ErrorCode::kMisuse, "aggregate state is finalized"));
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }

  try {
    Result<SqlValue> result = SqlValue{};
    if (state.impl_ == nullptr) {
      if (kind_ == Kind::kCount) {
        result = SqlValue::Integer(0);
      } else if (kind_ == Kind::kTotal) {
        result = SqlValue::Real(0.0);
      }
    } else {
      switch (kind_) {
        case Kind::kCount: {
          const auto* payload = std::get_if<CountAggregateState>(&state.impl_->payload);
          if (payload == nullptr) {
            result = std::unexpected(
                Error::Create(ErrorCode::kInternal, "aggregate state payload is invalid"));
          } else {
            result = SqlValue::Integer(payload->count);
          }
          break;
        }
        case Kind::kSum:
        case Kind::kTotal:
        case Kind::kAverage: {
          const auto* payload = std::get_if<NumericAggregateState>(&state.impl_->payload);
          if (payload == nullptr) {
            result = std::unexpected(
                Error::Create(ErrorCode::kInternal, "aggregate state payload is invalid"));
            break;
          }
          if (payload->count == 0 && (kind_ == Kind::kSum || kind_ == Kind::kAverage)) {
            result = SqlValue{};
          } else if (kind_ == Kind::kSum && payload->approximate && payload->overflow) {
            result = std::unexpected(Error::Create(ErrorCode::kGeneric, "integer overflow"));
          } else if (kind_ == Kind::kSum && !payload->approximate) {
            result = SqlValue::Integer(payload->integer_sum);
          } else {
            double numeric = NumericAggregateResult(*payload);
            if (kind_ == Kind::kAverage) {
              numeric /= static_cast<double>(payload->count);
            }
            result = SqlValue::Real(numeric);
          }
          break;
        }
        case Kind::kMinimum:
        case Kind::kMaximum: {
          auto* payload = std::get_if<MinMaxAggregateState>(&state.impl_->payload);
          if (payload == nullptr) {
            result = std::unexpected(
                Error::Create(ErrorCode::kInternal, "aggregate state payload is invalid"));
          } else if (payload->best.has_value()) {
            result = std::move(*payload->best);
          }
          break;
        }
        case Kind::kGroupConcat: {
          auto* payload = std::get_if<GroupConcatAggregateState>(&state.impl_->payload);
          if (payload == nullptr) {
            result = std::unexpected(
                Error::Create(ErrorCode::kInternal, "aggregate state payload is invalid"));
          } else if (payload->error.has_value()) {
            result = std::unexpected(std::move(*payload->error));
          } else if (payload->has_value) {
            result = SqlValue::Text(std::move(payload->text));
          }
          break;
        }
      }
    }
    state.impl_.reset();
    state.finalized_ = true;
    return result;
  } catch (const std::bad_alloc&) {
    state.impl_.reset();
    state.finalized_ = true;
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    state.impl_.reset();
    state.finalized_ = true;
    return std::unexpected(Error::OutOfMemory());
  }
}

ColumnArgumentUse ScalarFunction::column_argument_use() const noexcept {
  if (callback_ == TypeofFunction) {
    return ColumnArgumentUse::StorageType;
  }
  if (callback_ == LengthFunction) {
    return ColumnArgumentUse::BlobLength;
  }
  return ColumnArgumentUse::FullValue;
}

Result<std::optional<SqlValue>> ScalarFunction::InvokeColumnMetadata(SqlValueType type,
                                                                     ByteCount size) const {
  switch (type) {
    case SqlValueType::kNull:
    case SqlValueType::kInteger:
    case SqlValueType::kReal:
    case SqlValueType::kText:
    case SqlValueType::kBlob:
      break;
    default:
      return std::unexpected(Error::Create(ErrorCode::kMisuse, "invalid column storage class"));
  }
  const ColumnArgumentUse use = column_argument_use();
  if (use == ColumnArgumentUse::StorageType ||
      (use == ColumnArgumentUse::BlobLength && type == SqlValueType::kBlob)) {
    auto result =
        use == ColumnArgumentUse::StorageType ? TypeofResult(type) : LengthResult(size.value());
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    return std::optional<SqlValue>{std::move(*result)};
  }
  return std::optional<SqlValue>{};
}

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

Result<const AggregateFunction*> FunctionRegistry::ResolveAggregate(
    std::string_view name, std::size_t argument_count) const {
  const AggregateFunction* best = nullptr;
  bool name_found = false;
  for (const AggregateFunction& function : aggregate_functions_) {
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

const FunctionRegistry& CoreFunctionRegistry() noexcept {
  static constexpr std::array kCoreAggregateFunctions{
      AggregateFunction{"count", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kCount},
      AggregateFunction{"count", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kCount},
      AggregateFunction{"sum", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kSum},
      AggregateFunction{"total", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kTotal},
      AggregateFunction{"avg", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kAverage},
      AggregateFunction{"min", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kRequired, AggregateExtremumKind::kMinimum,
                        AggregateFunction::Kind::kMinimum},
      AggregateFunction{"max", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kRequired, AggregateExtremumKind::kMaximum,
                        AggregateFunction::Kind::kMaximum},
      AggregateFunction{"group_concat", FunctionArity::Exact(1),
                        FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                        AggregateExtremumKind::kNone, AggregateFunction::Kind::kGroupConcat},
      AggregateFunction{"group_concat", FunctionArity::Exact(2),
                        FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                        AggregateExtremumKind::kNone, AggregateFunction::Kind::kGroupConcat},
      AggregateFunction{"string_agg", FunctionArity::Exact(2), FunctionDeterminism::kDeterministic,
                        FunctionCollationUse::kNone, AggregateExtremumKind::kNone,
                        AggregateFunction::Kind::kGroupConcat},
  };
  static constexpr FunctionRegistry kCoreRegistry{
      std::span<const ScalarFunction>{kCoreFunctions},
      std::span<const AggregateFunction>{kCoreAggregateFunctions},
  };
  return kCoreRegistry;
}

}  // namespace modern_sqlite
