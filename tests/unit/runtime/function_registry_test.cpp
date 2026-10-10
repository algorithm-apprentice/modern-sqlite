#include "modern_sqlite/runtime/function_registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

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
