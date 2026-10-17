#include "modern_sqlite/runtime/function_registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<AggregateState>);
static_assert(!std::is_copy_assignable_v<AggregateState>);
static_assert(std::is_nothrow_move_constructible_v<AggregateState>);
static_assert(std::is_nothrow_move_assignable_v<AggregateState>);
static_assert(!std::is_copy_constructible_v<AggregateFunction>);
static_assert(!std::is_copy_assignable_v<AggregateFunction>);
static_assert(!std::is_move_constructible_v<AggregateFunction>);
static_assert(!std::is_move_assignable_v<AggregateFunction>);

[[nodiscard]] std::string TextFromBytes(std::initializer_list<std::uint8_t> bytes) {
  std::string result;
  result.reserve(bytes.size());
  for (const std::uint8_t byte : bytes) {
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

[[nodiscard]] SqlValue TextValue(std::string_view text) {
  return SqlValue::Text(std::string{text});
}

[[nodiscard]] SqlValue BlobValue(std::string_view bytes) {
  return SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(bytes)));
}

[[nodiscard]] Result<SqlValue> InvokeCore(std::string_view name,
                                          std::span<const SqlValue> arguments,
                                          const Collation& collation = BinaryCollation()) {
  auto function = CoreFunctionRegistry().Resolve(name, arguments.size());
  if (!function.has_value()) {
    return std::unexpected(std::move(function.error()));
  }
  return (*function)->Invoke(ScalarFunctionContext{collation}, arguments);
}

[[nodiscard]] const AggregateFunction& ResolveAggregate(std::string_view name,
                                                        std::size_t argument_count) {
  auto function = CoreFunctionRegistry().ResolveAggregate(name, argument_count);
  if (!function.has_value()) {
    throw std::runtime_error(function.error().ToString());
  }
  return **function;
}

AggregateStepEffect StepAggregate(const AggregateFunction& function, AggregateState& state,
                                  std::span<const SqlValue> arguments,
                                  const Collation& collation = BinaryCollation(),
                                  ByteCount maximum_value_bytes = ByteCount{1'000'000'000}) {
  auto stepped =
      function.Step(state, AggregateFunctionContext{collation, maximum_value_bytes}, arguments);
  if (!stepped.has_value()) {
    throw std::runtime_error(stepped.error().ToString());
  }
  return *stepped;
}

AggregateStepEffect StepOne(const AggregateFunction& function, AggregateState& state,
                            SqlValue value, const Collation& collation = BinaryCollation(),
                            ByteCount maximum_value_bytes = ByteCount{1'000'000'000}) {
  const std::array arguments{std::move(value)};
  return StepAggregate(function, state, arguments, collation, maximum_value_bytes);
}

AggregateStepEffect StepTwo(const AggregateFunction& function, AggregateState& state,
                            SqlValue first, SqlValue second,
                            ByteCount maximum_value_bytes = ByteCount{1'000'000'000}) {
  const std::array arguments{std::move(first), std::move(second)};
  return StepAggregate(function, state, arguments, BinaryCollation(), maximum_value_bytes);
}

void ExpectNull(const Result<SqlValue>& result) {
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(SqlValueType::kNull, result->type());
}

void ExpectInteger(const Result<SqlValue>& result, std::int64_t expected) {
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(SqlValueType::kInteger, result->type());
  ASSERT_TRUE(result->integer_value().has_value());
  EXPECT_EQ(expected, *result->integer_value());
}

void ExpectReal(const Result<SqlValue>& result, double expected) {
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(SqlValueType::kReal, result->type());
  ASSERT_TRUE(result->real_value().has_value());
  EXPECT_EQ(expected, *result->real_value());
}

void ExpectText(const Result<SqlValue>& result, std::string_view expected) {
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(SqlValueType::kText, result->type());
  ASSERT_TRUE(result->text_value().has_value());
  EXPECT_EQ(expected, result->text_value()->bytes());
}

[[nodiscard]] Result<SqlValue> ReturnExact(const ScalarFunctionContext&,
                                           std::span<const SqlValue>) {
  return SqlValue::Integer(20);
}

[[nodiscard]] Result<SqlValue> ReturnArgumentCount(const ScalarFunctionContext&,
                                                   std::span<const SqlValue> arguments) {
  return SqlValue::Integer(static_cast<std::int64_t>(arguments.size()));
}

[[nodiscard]] Result<SqlValue> ReturnNull(const ScalarFunctionContext&, std::span<const SqlValue>) {
  return SqlValue{};
}

TEST(FunctionRegistryTest, ResolvesNamesAndPrefersExactArity) {
  const std::array functions{
      ScalarFunction{"mixed", FunctionArity::AtLeast(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnArgumentCount},
      ScalarFunction{"mixed", FunctionArity::Exact(2), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnExact},
  };
  const FunctionRegistry registry{std::span<const ScalarFunction>{functions}};

  const auto exact = registry.Resolve("MiXeD", 2);
  const auto variadic = registry.Resolve("MIXED", 3);

  ASSERT_TRUE(exact.has_value());
  ASSERT_TRUE(variadic.has_value());
  EXPECT_TRUE((*exact)->arity().is_exact());
  EXPECT_FALSE((*variadic)->arity().is_exact());

  const std::array exact_arguments{SqlValue{}, SqlValue{}};
  const std::array variadic_arguments{SqlValue{}, SqlValue{}, SqlValue{}};
  ExpectInteger((*exact)->Invoke(ScalarFunctionContext{BinaryCollation()}, exact_arguments), 20);
  ExpectInteger((*variadic)->Invoke(ScalarFunctionContext{BinaryCollation()}, variadic_arguments),
                3);
}

TEST(FunctionRegistryTest, FoldsOnlyAsciiBytesInFunctionNames) {
  const std::string registered_name = TextFromBytes({0x46, 0xc0});
  const std::string folded_ascii_name = TextFromBytes({0x66, 0xc0});
  const std::string different_high_byte = TextFromBytes({0x66, 0xe0});
  const std::array functions{
      ScalarFunction{registered_name, FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnNull},
  };
  const FunctionRegistry registry{std::span<const ScalarFunction>{functions}};

  EXPECT_TRUE(registry.Resolve(folded_ascii_name, 0).has_value());
  const auto different = registry.Resolve(different_high_byte, 0);
  ASSERT_FALSE(different.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, different.error().code());
}

TEST(FunctionRegistryTest, ReportsUnknownNamesWrongArityAndDirectMisuse) {
  const std::array functions{
      ScalarFunction{"known", FunctionArity::Exact(1), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ReturnNull},
  };
  const FunctionRegistry registry{std::span<const ScalarFunction>{functions}};

  const auto missing = registry.Resolve("absent", 1);
  const auto wrong_arity = registry.Resolve("KNOWN", 2);
  const auto known = registry.Resolve("known", 1);

  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
  EXPECT_EQ("no such function: absent", missing.error().message());
  ASSERT_FALSE(wrong_arity.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, wrong_arity.error().code());
  EXPECT_EQ("wrong number of arguments to function KNOWN()", wrong_arity.error().message());
  ASSERT_TRUE(known.has_value());

  const auto misuse =
      (*known)->Invoke(ScalarFunctionContext{BinaryCollation()}, std::span<const SqlValue>{});
  ASSERT_FALSE(misuse.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, misuse.error().code());
  EXPECT_EQ("wrong number of arguments to function known()", misuse.error().message());
}

TEST(FunctionRegistryTest, ExposesDeterminismCollationAndArityMetadata) {
  struct ExpectedFunction {
    std::string_view name;
    std::size_t argument_count;
    bool exact;
    bool uses_collation;
  };
  constexpr std::array expected{
      ExpectedFunction{
          .name = "typeof",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "length",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "abs",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "lower",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "upper",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "sign",
          .argument_count = 1,
          .exact = true,
          .uses_collation = false,
      },
      ExpectedFunction{
          .name = "nullif",
          .argument_count = 2,
          .exact = true,
          .uses_collation = true,
      },
      ExpectedFunction{
          .name = "min",
          .argument_count = 2,
          .exact = false,
          .uses_collation = true,
      },
      ExpectedFunction{
          .name = "max",
          .argument_count = 2,
          .exact = false,
          .uses_collation = true,
      },
  };

  for (const ExpectedFunction& expected_function : expected) {
    SCOPED_TRACE(expected_function.name);
    const auto function =
        CoreFunctionRegistry().Resolve(expected_function.name, expected_function.argument_count);
    ASSERT_TRUE(function.has_value());
    EXPECT_TRUE((*function)->is_deterministic());
    EXPECT_EQ(expected_function.exact, (*function)->arity().is_exact());
    EXPECT_EQ(expected_function.argument_count, (*function)->arity().minimum_argument_count());
    EXPECT_EQ(expected_function.uses_collation, (*function)->uses_collation());
  }

  const std::array functions{
      ScalarFunction{"volatile", FunctionArity::Exact(0), FunctionDeterminism::kNonDeterministic,
                     FunctionCollationUse::kNone, ReturnNull},
  };
  const FunctionRegistry registry{std::span<const ScalarFunction>{functions}};
  const auto volatile_function = registry.Resolve("volatile", 0);
  ASSERT_TRUE(volatile_function.has_value());
  EXPECT_FALSE((*volatile_function)->is_deterministic());
}

TEST(FunctionRegistryTest, KeepsLazySpecialFormsOutOfEagerScalarDispatch) {
  for (const std::string_view name :
       {"coalesce", "ifnull", "iif", "if", "likely", "likelihood", "unlikely"}) {
    SCOPED_TRACE(name);
    const auto function = CoreFunctionRegistry().Resolve(name, 2);
    ASSERT_FALSE(function.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, function.error().code());
    EXPECT_EQ("no such function: " + std::string{name}, function.error().message());
  }
}

TEST(FunctionRegistryTest, KeepsInstrumentationProbeOutOfOrdinaryRegistry) {
  const auto function = CoreFunctionRegistry().Resolve("modern_sqlite_probe", 2);

  ASSERT_FALSE(function.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, function.error().code());
  EXPECT_EQ("no such function: modern_sqlite_probe", function.error().message());
}

TEST(AggregateFunctionRegistryTest, ResolvesCountAndOwnsLazyState) {
  const auto function = CoreFunctionRegistry().ResolveAggregate("COUNT", 0);
  ASSERT_TRUE(function.has_value());
  EXPECT_EQ("count", (*function)->name());
  EXPECT_TRUE((*function)->is_deterministic());
  EXPECT_FALSE((*function)->uses_collation());
  EXPECT_EQ(AggregateExtremumKind::kNone, (*function)->extremum_kind());

  AggregateState state = (*function)->CreateState();
  EXPECT_TRUE(state.valid());
  EXPECT_FALSE(state.initialized());
  EXPECT_FALSE(state.finalized());

  const auto stepped =
      (*function)->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                        std::span<const SqlValue>{});
  ASSERT_TRUE(stepped.has_value());
  EXPECT_EQ(AggregateStepEffect::kNoRepresentativePreference, *stepped);
  EXPECT_TRUE(state.initialized());

  ExpectInteger((*function)->Finalize(state), 1);
  EXPECT_TRUE(state.finalized());
  state.Reset();
  EXPECT_FALSE(state.initialized());
  EXPECT_FALSE(state.finalized());
}

TEST(AggregateFunctionRegistryTest, PublishesCoreMetadataAndSeparateScalarResolution) {
  struct ExpectedAggregate {
    std::string_view name;
    std::size_t argument_count;
    bool uses_collation;
    AggregateExtremumKind extremum;
  };
  constexpr std::array expected{
      ExpectedAggregate{.name = "count",
                        .argument_count = 0,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "count",
                        .argument_count = 1,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "sum",
                        .argument_count = 1,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "total",
                        .argument_count = 1,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "avg",
                        .argument_count = 1,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "min",
                        .argument_count = 1,
                        .uses_collation = true,
                        .extremum = AggregateExtremumKind::kMinimum},
      ExpectedAggregate{.name = "max",
                        .argument_count = 1,
                        .uses_collation = true,
                        .extremum = AggregateExtremumKind::kMaximum},
      ExpectedAggregate{.name = "group_concat",
                        .argument_count = 1,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "group_concat",
                        .argument_count = 2,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
      ExpectedAggregate{.name = "string_agg",
                        .argument_count = 2,
                        .uses_collation = false,
                        .extremum = AggregateExtremumKind::kNone},
  };

  EXPECT_EQ(expected.size(), CoreFunctionRegistry().aggregate_functions().size());
  for (const ExpectedAggregate& expected_function : expected) {
    SCOPED_TRACE(expected_function.name);
    const auto resolved = CoreFunctionRegistry().ResolveAggregate(expected_function.name,
                                                                  expected_function.argument_count);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE((*resolved)->arity().is_exact());
    EXPECT_TRUE((*resolved)->is_deterministic());
    EXPECT_EQ(expected_function.uses_collation, (*resolved)->uses_collation());
    EXPECT_EQ(expected_function.extremum, (*resolved)->extremum_kind());
  }

  EXPECT_TRUE(CoreFunctionRegistry().Resolve("min", 2).has_value());
  EXPECT_TRUE(CoreFunctionRegistry().ResolveAggregate("min", 1).has_value());
  const auto scalar_min = CoreFunctionRegistry().Resolve("min", 1);
  const auto aggregate_min = CoreFunctionRegistry().ResolveAggregate("min", 2);
  ASSERT_FALSE(scalar_min.has_value());
  ASSERT_FALSE(aggregate_min.has_value());
  EXPECT_EQ("wrong number of arguments to function min()", scalar_min.error().message());
  EXPECT_EQ("wrong number of arguments to function min()", aggregate_min.error().message());

  const auto missing = CoreFunctionRegistry().ResolveAggregate("missing", 1);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ("no such function: missing", missing.error().message());
}

TEST(AggregateFunctionStateTest, EnforcesMoveDescriptorAndFinalizationLifecycle) {
  const AggregateFunction& count = ResolveAggregate("count", 0);
  const AggregateFunction& sum = ResolveAggregate("sum", 1);
  AggregateState state = count.CreateState();
  AggregateState moved{std::move(state)};
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(state.valid());
  EXPECT_TRUE(moved.valid());

  const auto invalid_step =
      count.Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                 std::span<const SqlValue>{});
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  ASSERT_FALSE(invalid_step.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_step.error().code());

  const std::array one{SqlValue::Integer(1)};
  const auto wrong_function =
      sum.Step(moved, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}}, one);
  ASSERT_FALSE(wrong_function.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, wrong_function.error().code());

  EXPECT_EQ(AggregateStepEffect::kNoRepresentativePreference,
            StepAggregate(count, moved, std::span<const SqlValue>{}));
  ExpectInteger(count.Finalize(moved), 1);
  const auto finalized_step =
      count.Step(moved, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                 std::span<const SqlValue>{});
  const auto finalized_again = count.Finalize(moved);
  ASSERT_FALSE(finalized_step.has_value());
  ASSERT_FALSE(finalized_again.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, finalized_step.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, finalized_again.error().code());

  moved.Reset();
  EXPECT_FALSE(moved.initialized());
  EXPECT_FALSE(moved.finalized());
  ExpectInteger(count.Finalize(moved), 0);

  AggregateState wrong_arity_state = count.CreateState();
  const auto wrong_arity = count.Step(
      wrong_arity_state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}}, one);
  ASSERT_FALSE(wrong_arity.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, wrong_arity.error().code());
}

TEST(CoreAggregateTest, CountHandlesEmptyNullAndZeroArgumentInputs) {
  const AggregateFunction& count_all = ResolveAggregate("count", 0);
  const AggregateFunction& count_value = ResolveAggregate("count", 1);

  AggregateState empty = count_all.CreateState();
  ExpectInteger(count_all.Finalize(empty), 0);

  AggregateState all = count_all.CreateState();
  StepAggregate(count_all, all, std::span<const SqlValue>{});
  StepAggregate(count_all, all, std::span<const SqlValue>{});
  ExpectInteger(count_all.Finalize(all), 2);

  AggregateState values = count_value.CreateState();
  EXPECT_FALSE(values.initialized());
  StepOne(count_value, values, SqlValue{});
  EXPECT_TRUE(values.initialized());
  StepOne(count_value, values, SqlValue::Integer(1));
  StepOne(count_value, values, TextValue("x"));
  ExpectInteger(count_value.Finalize(values), 2);
}

TEST(CoreAggregateTest, NumericTextClassificationUsesFirstNullAndFullIntegerFallback) {
  const AggregateFunction& sum = ResolveAggregate("sum", 1);

  AggregateState small = sum.CreateState();
  StepOne(sum, small, TextValue(std::string_view{"1\0e2", 4}));
  ExpectInteger(sum.Finalize(small), 1);

  AggregateState large = sum.CreateState();
  StepOne(sum, large, TextValue(std::string_view{"9007199254740992\0x", 18}));
  ExpectReal(sum.Finalize(large), 9007199254740992.0);
}

TEST(CoreAggregateTest, NumericTextClassificationPreservesAndClearsOverflowLatch) {
  const AggregateFunction& sum = ResolveAggregate("sum", 1);
  for (const bool negative : {false, true}) {
    SCOPED_TRACE(negative);
    const std::int64_t first = negative ? std::numeric_limits<std::int64_t>::min()
                                        : std::numeric_limits<std::int64_t>::max();
    const std::int64_t second = negative ? -1 : 1;

    AggregateState retained = sum.CreateState();
    StepOne(sum, retained, SqlValue::Integer(first));
    StepOne(sum, retained, SqlValue::Integer(second));
    StepOne(sum, retained, TextValue(std::string_view{"1\0e2", 4}));
    const auto overflow = sum.Finalize(retained);
    EXPECT_FALSE(overflow.has_value());
    if (!overflow.has_value()) {
      EXPECT_EQ(ErrorCode::kGeneric, overflow.error().code());
      EXPECT_EQ("integer overflow", overflow.error().message());
    }
    EXPECT_TRUE(retained.finalized());
    EXPECT_FALSE(retained.initialized());

    AggregateState cleared = sum.CreateState();
    StepOne(sum, cleared, SqlValue::Integer(first));
    StepOne(sum, cleared, SqlValue::Integer(second));
    StepOne(sum, cleared, TextValue(std::string_view{"2251799813685248\0x", 18}));
    const auto real = sum.Finalize(cleared);
    ExpectReal(real, static_cast<double>(first) + 2251799813685248.0);
    EXPECT_TRUE(cleared.finalized());
    EXPECT_FALSE(cleared.initialized());
    cleared.Reset();
    StepOne(sum, cleared, SqlValue::Integer(2));
    ExpectInteger(sum.Finalize(cleared), 2);
  }
}

TEST(CoreAggregateTest, NumericAggregatesMatchSQLiteTypesOverflowAndCompensation) {
  const AggregateFunction& sum = ResolveAggregate("sum", 1);
  const AggregateFunction& total = ResolveAggregate("total", 1);
  const AggregateFunction& average = ResolveAggregate("avg", 1);

  AggregateState empty_sum = sum.CreateState();
  AggregateState empty_total = total.CreateState();
  AggregateState empty_average = average.CreateState();
  ExpectNull(sum.Finalize(empty_sum));
  ExpectReal(total.Finalize(empty_total), 0.0);
  ExpectNull(average.Finalize(empty_average));

  AggregateState null_sum = sum.CreateState();
  StepOne(sum, null_sum, SqlValue{});
  EXPECT_TRUE(null_sum.initialized());
  ExpectNull(sum.Finalize(null_sum));

  AggregateState integer_sum = sum.CreateState();
  StepOne(sum, integer_sum, TextValue("1"));
  StepOne(sum, integer_sum, TextValue("2"));
  ExpectInteger(sum.Finalize(integer_sum), 3);

  AggregateState decimal_text = sum.CreateState();
  StepOne(sum, decimal_text, TextValue("1.0"));
  ExpectReal(sum.Finalize(decimal_text), 1.0);

  AggregateState mixed_sum = sum.CreateState();
  StepOne(sum, mixed_sum, TextValue("1"));
  StepOne(sum, mixed_sum, TextValue("2.5"));
  StepOne(sum, mixed_sum, TextValue("bad"));
  StepOne(sum, mixed_sum, BlobValue("3"));
  ExpectReal(sum.Finalize(mixed_sum), 6.5);

  AggregateState overflow = sum.CreateState();
  StepOne(sum, overflow, SqlValue::Integer(std::numeric_limits<std::int64_t>::max()));
  StepOne(sum, overflow, SqlValue::Integer(1));
  const auto overflow_result = sum.Finalize(overflow);
  ASSERT_FALSE(overflow_result.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, overflow_result.error().code());
  EXPECT_EQ("integer overflow", overflow_result.error().message());
  EXPECT_TRUE(overflow.finalized());
  EXPECT_FALSE(overflow.initialized());
  overflow.Reset();

  AggregateState cleared = sum.CreateState();
  StepOne(sum, cleared, SqlValue::Integer(std::numeric_limits<std::int64_t>::max()));
  StepOne(sum, cleared, SqlValue::Integer(1));
  StepOne(sum, cleared, SqlValue::Real(0.0));
  const auto cleared_result = sum.Finalize(cleared);
  ASSERT_TRUE(cleared_result.has_value());
  ASSERT_TRUE(cleared_result->real_value().has_value());
  EXPECT_DOUBLE_EQ(9.2233720368547758e+18, *cleared_result->real_value());

  AggregateState text_cleared = sum.CreateState();
  StepOne(sum, text_cleared, SqlValue::Integer(std::numeric_limits<std::int64_t>::max()));
  StepOne(sum, text_cleared, SqlValue::Integer(1));
  StepOne(sum, text_cleared, TextValue("1e0"));
  const auto text_cleared_result = sum.Finalize(text_cleared);
  ASSERT_TRUE(text_cleared_result.has_value());
  EXPECT_EQ(SqlValueType::kReal, text_cleared_result->type());

  AggregateState total_overflow = total.CreateState();
  AggregateState average_overflow = average.CreateState();
  for (const std::int64_t value : {std::numeric_limits<std::int64_t>::max(), INT64_C(1)}) {
    StepOne(total, total_overflow, SqlValue::Integer(value));
    StepOne(average, average_overflow, SqlValue::Integer(value));
  }
  ExpectReal(total.Finalize(total_overflow), 9.2233720368547758e+18);
  ExpectReal(average.Finalize(average_overflow), 4.6116860184273879e+18);

  AggregateState compensated = sum.CreateState();
  StepOne(sum, compensated, SqlValue::Real(1e16));
  StepOne(sum, compensated, SqlValue::Real(1.0));
  StepOne(sum, compensated, SqlValue::Real(-1e16));
  ExpectReal(sum.Finalize(compensated), 1.0);

  AggregateState positive_infinity = sum.CreateState();
  StepOne(sum, positive_infinity, SqlValue::Real(std::numeric_limits<double>::infinity()));
  const auto positive_result = sum.Finalize(positive_infinity);
  ASSERT_TRUE(positive_result.has_value());
  ASSERT_TRUE(positive_result->real_value().has_value());
  EXPECT_TRUE(std::isinf(*positive_result->real_value()));

  AggregateState opposite_infinities = total.CreateState();
  StepOne(total, opposite_infinities, SqlValue::Real(std::numeric_limits<double>::infinity()));
  StepOne(total, opposite_infinities, SqlValue::Real(-std::numeric_limits<double>::infinity()));
  ExpectNull(total.Finalize(opposite_infinities));
}

TEST(CoreAggregateTest, MinMaxPreserveFirstTiesCollationAndRepresentativeEffects) {
  const AggregateFunction& minimum = ResolveAggregate("min", 1);
  const AggregateFunction& maximum = ResolveAggregate("max", 1);

  AggregateState all_null = minimum.CreateState();
  EXPECT_EQ(AggregateStepEffect::kReplaceRepresentative, StepOne(minimum, all_null, SqlValue{}));
  EXPECT_TRUE(all_null.initialized());
  EXPECT_EQ(AggregateStepEffect::kReplaceRepresentative, StepOne(minimum, all_null, SqlValue{}));
  ExpectNull(minimum.Finalize(all_null));

  AggregateState text_minimum = minimum.CreateState();
  EXPECT_EQ(AggregateStepEffect::kReplaceRepresentative,
            StepOne(minimum, text_minimum, TextValue("b"), NoCaseCollation()));
  EXPECT_EQ(AggregateStepEffect::kKeepRepresentative,
            StepOne(minimum, text_minimum, TextValue("B"), NoCaseCollation()));
  EXPECT_EQ(AggregateStepEffect::kReplaceRepresentative,
            StepOne(minimum, text_minimum, TextValue("a"), NoCaseCollation()));
  EXPECT_EQ(AggregateStepEffect::kKeepRepresentative,
            StepOne(minimum, text_minimum, SqlValue{}, NoCaseCollation()));
  ExpectText(minimum.Finalize(text_minimum), "a");

  AggregateState numeric_tie = maximum.CreateState();
  EXPECT_EQ(AggregateStepEffect::kReplaceRepresentative,
            StepOne(maximum, numeric_tie, SqlValue::Integer(1)));
  EXPECT_EQ(AggregateStepEffect::kKeepRepresentative,
            StepOne(maximum, numeric_tie, SqlValue::Real(1.0)));
  ExpectInteger(maximum.Finalize(numeric_tie), 1);

  const std::string payload(128, 'x');
  SqlValue argument = TextValue(payload);
  const std::optional<Utf8View> argument_text = argument.text_value();
  ASSERT_TRUE(argument_text.has_value());
  const char* const argument_data = argument_text->data();
  AggregateState cloned = minimum.CreateState();
  StepOne(minimum, cloned, std::move(argument));
  const auto result = minimum.Finalize(cloned);
  ExpectText(result, payload);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->text_value().has_value());
  EXPECT_NE(argument_data, result->text_value()->data());
}

TEST(CoreAggregateTest, GroupConcatMatchesSQLiteSeparatorsTypesLimitsAndLazyState) {
  const AggregateFunction& one_argument = ResolveAggregate("group_concat", 1);
  const AggregateFunction& two_arguments = ResolveAggregate("group_concat", 2);
  const AggregateFunction& string_agg = ResolveAggregate("string_agg", 2);

  AggregateState empty = one_argument.CreateState();
  ExpectNull(one_argument.Finalize(empty));

  AggregateState lazy = one_argument.CreateState();
  StepOne(one_argument, lazy, SqlValue{});
  EXPECT_FALSE(lazy.initialized());
  lazy.Reset();

  AggregateState default_separator = one_argument.CreateState();
  StepOne(one_argument, default_separator, TextValue("a"));
  StepOne(one_argument, default_separator, SqlValue{});
  StepOne(one_argument, default_separator, TextValue("b"));
  StepOne(one_argument, default_separator, TextValue(""));
  ExpectText(one_argument.Finalize(default_separator), "a,b,");

  AggregateState row_separators = two_arguments.CreateState();
  StepTwo(two_arguments, row_separators, TextValue("a"), TextValue("/"));
  StepTwo(two_arguments, row_separators, SqlValue{}, TextValue("ignored"));
  StepTwo(two_arguments, row_separators, TextValue("b"), SqlValue{});
  StepTwo(two_arguments, row_separators, TextValue(""), TextValue(":"));
  ExpectText(two_arguments.Finalize(row_separators), "ab:");

  AggregateState alias = string_agg.CreateState();
  StepTwo(string_agg, alias, TextValue("a"), TextValue("/"));
  StepTwo(string_agg, alias, TextValue("b"), TextValue("|"));
  ExpectText(string_agg.Finalize(alias), "a|b");

  AggregateState converted = two_arguments.CreateState();
  StepTwo(two_arguments, converted, SqlValue::Integer(1), TextValue("|"));
  StepTwo(two_arguments, converted, SqlValue::Real(1.25), TextValue("|"));
  StepTwo(two_arguments, converted, BlobValue(TextFromBytes({0x41, 0x00, 0x42})), TextValue("|"));
  ExpectText(two_arguments.Finalize(converted),
             TextFromBytes({0x31, 0x7c, 0x31, 0x2e, 0x32, 0x35, 0x7c, 0x41, 0x00, 0x42}));

  AggregateState exact_limit = one_argument.CreateState();
  StepOne(one_argument, exact_limit, TextValue("a"), BinaryCollation(), ByteCount{3});
  StepOne(one_argument, exact_limit, TextValue("b"), BinaryCollation(), ByteCount{3});
  ExpectText(one_argument.Finalize(exact_limit), "a,b");

  AggregateState unused_separator = two_arguments.CreateState();
  StepTwo(two_arguments, unused_separator, TextValue("a"), TextValue("oversized"), ByteCount{1});
  ExpectText(two_arguments.Finalize(unused_separator), "a");

  AggregateState empty_at_zero_limit = one_argument.CreateState();
  StepOne(one_argument, empty_at_zero_limit, TextValue(""), BinaryCollation(), ByteCount{0});
  ExpectText(one_argument.Finalize(empty_at_zero_limit), "");

  AggregateState exceeded = one_argument.CreateState();
  EXPECT_EQ(AggregateStepEffect::kNoRepresentativePreference,
            StepOne(one_argument, exceeded, TextValue("a"), BinaryCollation(), ByteCount{2}));
  EXPECT_EQ(AggregateStepEffect::kNoRepresentativePreference,
            StepOne(one_argument, exceeded, TextValue("b"), BinaryCollation(), ByteCount{2}));
  const auto too_large = one_argument.Finalize(exceeded);
  ASSERT_FALSE(too_large.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, too_large.error().code());
  EXPECT_EQ("string or blob too big", too_large.error().message());
}

TEST(CoreFunctionTest, TypeofReturnsSQLiteStorageClassNames) {
  const std::array null_argument{SqlValue{}};
  const std::array integer_argument{SqlValue::Integer(7)};
  const std::array real_argument{SqlValue::Real(1.5)};
  const std::array text_argument{TextValue("value")};
  const std::array blob_argument{BlobValue("value")};

  ExpectText(InvokeCore("typeof", null_argument), "null");
  ExpectText(InvokeCore("typeof", integer_argument), "integer");
  ExpectText(InvokeCore("typeof", real_argument), "real");
  ExpectText(InvokeCore("typeof", text_argument), "text");
  ExpectText(InvokeCore("typeof", blob_argument), "blob");
}

TEST(CoreFunctionTest, LengthMatchesSQLiteTextBlobAndNumericRules) {
  const std::array null_argument{SqlValue{}};
  const std::array integer_argument{SqlValue::Integer(123)};
  const std::array real_argument{SqlValue::Real(1.25)};
  const std::array blob_argument{BlobValue(TextFromBytes({0x61, 0x00, 0x62}))};
  const std::array text_with_null{TextValue(TextFromBytes({0x61, 0x00, 0x62}))};
  const std::array valid_utf8{TextValue(TextFromBytes({0x41, 0xc3, 0xa4, 0xf0, 0x9f, 0x98, 0x80}))};
  const std::array ascii_then_continuation{TextValue(TextFromBytes({0x41, 0x80}))};
  const std::array continuation_run{TextValue(TextFromBytes({0x80, 0x80}))};
  const std::array malformed_lead{TextValue(TextFromBytes({0xc0, 0x80}))};
  const std::array out_of_range_lead{TextValue(TextFromBytes({0xff, 0x80}))};

  ExpectNull(InvokeCore("length", null_argument));
  ExpectInteger(InvokeCore("length", integer_argument), 3);
  ExpectInteger(InvokeCore("length", real_argument), 4);
  ExpectInteger(InvokeCore("length", blob_argument), 3);
  ExpectInteger(InvokeCore("length", text_with_null), 1);
  ExpectInteger(InvokeCore("length", valid_utf8), 3);
  ExpectInteger(InvokeCore("length", ascii_then_continuation), 2);
  ExpectInteger(InvokeCore("length", continuation_run), 1);
  ExpectInteger(InvokeCore("length", malformed_lead), 1);
  ExpectInteger(InvokeCore("length", out_of_range_lead), 1);
}

TEST(CoreFunctionTest, AbsPropagatesNullConvertsInputsAndReportsOverflow) {
  const std::array null_argument{SqlValue{}};
  const std::array integer_argument{SqlValue::Integer(-7)};
  const std::array real_argument{SqlValue::Real(-1.25)};
  const std::array text_argument{TextValue("  -2.5x")};
  const std::array blob_argument{BlobValue("2")};
  const std::array invalid_argument{TextValue("not numeric")};
  const std::array negative_zero_argument{SqlValue::Real(-0.0)};
  const std::array overflow_argument{SqlValue::Integer(std::numeric_limits<std::int64_t>::min())};

  ExpectNull(InvokeCore("abs", null_argument));
  ExpectInteger(InvokeCore("abs", integer_argument), 7);
  ExpectReal(InvokeCore("abs", real_argument), 1.25);
  ExpectReal(InvokeCore("abs", text_argument), 2.5);
  ExpectReal(InvokeCore("abs", blob_argument), 2.0);
  ExpectReal(InvokeCore("abs", invalid_argument), 0.0);

  const auto negative_zero = InvokeCore("abs", negative_zero_argument);
  ASSERT_TRUE(negative_zero.has_value());
  ASSERT_TRUE(negative_zero->real_value().has_value());
  EXPECT_TRUE(std::signbit(*negative_zero->real_value()));

  const auto overflow = InvokeCore("abs", overflow_argument);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, overflow.error().code());
  EXPECT_EQ("integer overflow", overflow.error().message());
}

TEST(CoreFunctionTest, LowerAndUpperUseAsciiOnlyAndReturnText) {
  const std::array null_argument{SqlValue{}};
  const std::array lower_argument{TextValue(TextFromBytes({0x41, 0x00, 0x42, 0xc3, 0x84, 0xff}))};
  const std::array upper_argument{TextValue(TextFromBytes({0x61, 0x00, 0x62, 0xc3, 0xa4, 0xff}))};
  const std::array integer_argument{SqlValue::Integer(123)};
  const std::array real_argument{SqlValue::Real(1.25)};
  const std::array blob_argument{BlobValue("AB")};

  ExpectNull(InvokeCore("lower", null_argument));
  ExpectNull(InvokeCore("upper", null_argument));
  ExpectText(InvokeCore("lower", lower_argument),
             TextFromBytes({0x61, 0x00, 0x62, 0xc3, 0x84, 0xff}));
  ExpectText(InvokeCore("upper", upper_argument),
             TextFromBytes({0x41, 0x00, 0x42, 0xc3, 0xa4, 0xff}));
  ExpectText(InvokeCore("lower", integer_argument), "123");
  ExpectText(InvokeCore("upper", real_argument), "1.25");
  ExpectText(InvokeCore("lower", blob_argument), "ab");
}

TEST(CoreFunctionTest, SignUsesLosslessNumericAffinity) {
  const std::array null_argument{SqlValue{}};
  const std::array negative_argument{SqlValue::Integer(-2)};
  const std::array negative_zero_argument{SqlValue::Real(-0.0)};
  const std::array numeric_text{TextValue("2.0")};
  const std::array invalid_text{TextValue("2x")};
  const std::array numeric_blob{BlobValue("2")};

  ExpectNull(InvokeCore("sign", null_argument));
  ExpectInteger(InvokeCore("sign", negative_argument), -1);
  ExpectInteger(InvokeCore("sign", negative_zero_argument), 0);
  ExpectInteger(InvokeCore("sign", numeric_text), 1);
  ExpectNull(InvokeCore("sign", invalid_text));
  ExpectNull(InvokeCore("sign", numeric_blob));
}

TEST(CoreFunctionTest, NullifUsesSelectedCollationAndClonesReturnedValues) {
  const std::array binary_arguments{TextValue("a"), TextValue("A")};
  const std::array numeric_arguments{SqlValue::Integer(1), SqlValue::Real(1.0)};
  const std::array null_arguments{SqlValue{}, SqlValue{}};
  const std::string payload(128, 'x');
  const std::array clone_arguments{TextValue(payload), TextValue("different")};
  const auto argument_text = clone_arguments[0].text_value();
  ASSERT_TRUE(argument_text.has_value());
  const char* const argument_data = argument_text->data();

  ExpectText(InvokeCore("nullif", binary_arguments), "a");
  ExpectNull(InvokeCore("nullif", binary_arguments, NoCaseCollation()));
  ExpectNull(InvokeCore("nullif", numeric_arguments));
  ExpectNull(InvokeCore("nullif", null_arguments));

  const auto cloned = InvokeCore("nullif", clone_arguments);
  ExpectText(cloned, payload);
  ASSERT_TRUE(cloned.has_value());
  ASSERT_TRUE(cloned->text_value().has_value());
  EXPECT_NE(argument_data, cloned->text_value()->data());
}

TEST(CoreFunctionTest, ScalarMinAndMaxMatchSQLiteNullTieAndCollationRules) {
  const std::array minimum_arguments{SqlValue::Integer(2), SqlValue::Integer(1),
                                     SqlValue::Real(1.0)};
  const std::array maximum_arguments{SqlValue::Integer(1), SqlValue::Integer(2),
                                     SqlValue::Real(2.0)};
  const std::array null_arguments{SqlValue::Integer(1), SqlValue{}, SqlValue::Integer(2)};
  const std::array text_arguments{TextValue("b"), TextValue("A"), TextValue("a")};
  const std::array equivalent_maximum{TextValue("a"), TextValue("A")};

  ExpectReal(InvokeCore("min", minimum_arguments), 1.0);
  ExpectInteger(InvokeCore("max", maximum_arguments), 2);
  ExpectNull(InvokeCore("min", null_arguments));
  ExpectNull(InvokeCore("max", null_arguments));
  ExpectText(InvokeCore("min", text_arguments, NoCaseCollation()), "a");
  ExpectText(InvokeCore("max", equivalent_maximum, NoCaseCollation()), "a");

  const auto one_argument = CoreFunctionRegistry().Resolve("min", 1);
  ASSERT_FALSE(one_argument.has_value());
  EXPECT_EQ("wrong number of arguments to function min()", one_argument.error().message());
}

}  // namespace
}  // namespace modern_sqlite
