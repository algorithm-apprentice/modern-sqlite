#include "modern_sqlite/runtime/sql_value.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<SqlValue>);
static_assert(!std::is_copy_assignable_v<SqlValue>);
static_assert(std::is_nothrow_move_constructible_v<SqlValue>);
static_assert(std::is_nothrow_move_assignable_v<SqlValue>);

[[nodiscard]] SqlValue TextValue(std::string_view text) {
  return SqlValue::Text(std::string{text});
}

[[nodiscard]] SqlValue BlobValue(std::string_view bytes) {
  return SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(bytes)));
}

void ExpectNull(const SqlValue& value) { EXPECT_EQ(SqlValueType::kNull, value.type()); }

void ExpectInteger(const SqlValue& value, std::int64_t expected) {
  ASSERT_EQ(SqlValueType::kInteger, value.type());
  ASSERT_TRUE(value.integer_value().has_value());
  EXPECT_EQ(expected, *value.integer_value());
}

void ExpectReal(const SqlValue& value, double expected) {
  ASSERT_EQ(SqlValueType::kReal, value.type());
  ASSERT_TRUE(value.real_value().has_value());
  EXPECT_EQ(expected, *value.real_value());
}

void ExpectText(const SqlValue& value, std::string_view expected) {
  ASSERT_EQ(SqlValueType::kText, value.type());
  ASSERT_TRUE(value.text_value().has_value());
  EXPECT_EQ(expected, value.text_value()->bytes());
}

void ExpectBlob(const SqlValue& value, std::string_view expected) {
  ASSERT_EQ(SqlValueType::kBlob, value.type());
  ASSERT_TRUE(value.blob_value().has_value());
  EXPECT_EQ(expected, AsStringView(*value.blob_value()));
}

TEST(SqlValue, RepresentsEveryStorageClassAndPreservesOwnedBytes) {
  const std::string text{"a\0b", 3};
  const std::string blob{"\xff\0z", 3};
  const SqlValue null_value;
  const SqlValue integer_value = SqlValue::Integer(-42);
  const SqlValue real_value = SqlValue::Real(1.25);
  const SqlValue text_value = TextValue(text);
  const SqlValue blob_value = BlobValue(blob);

  ExpectNull(null_value);
  ExpectInteger(integer_value, -42);
  ExpectReal(real_value, 1.25);
  ExpectText(text_value, text);
  ExpectBlob(blob_value, blob);
}

TEST(SqlValue, ClonesTextAndBlobOnlyWhenExplicitlyRequested) {
  const SqlValue text = TextValue("a text payload long enough to avoid small-string storage");
  const SqlValue blob = BlobValue("a blob payload long enough to require vector storage");

  const SqlValue text_clone = text.Clone();
  const SqlValue blob_clone = blob.Clone();

  const auto text_view = text.text_value();
  const auto cloned_text_view = text_clone.text_value();
  const auto blob_view = blob.blob_value();
  const auto cloned_blob_view = blob_clone.blob_value();
  ASSERT_TRUE(text_view.has_value());
  ASSERT_TRUE(cloned_text_view.has_value());
  ASSERT_TRUE(blob_view.has_value());
  ASSERT_TRUE(cloned_blob_view.has_value());
  ExpectText(text_clone, text_view->bytes());
  ExpectBlob(blob_clone, AsStringView(*blob_view));
  EXPECT_NE(text_view->data(), cloned_text_view->data());
  EXPECT_NE(blob_view->data(), cloned_blob_view->data());
}

TEST(SqlValue, ReportsRetainedDynamicStorageCapacity) {
  std::string text;
  text.reserve(1024);
  text = "payload";
  const SqlValue text_value = SqlValue::Text(std::move(text));
  const SqlValue blob_value = BlobValue("blob");

  EXPECT_GE(text_value.owned_capacity_bytes(), 1024U);
  EXPECT_GE(blob_value.owned_capacity_bytes(), 4U);
  EXPECT_EQ(SqlValue::Integer(1).owned_capacity_bytes(), 0U);
}

TEST(SqlValue, NormalizesNaNToNullAndPreservesInfinityAndSignedZero) {
  const SqlValue nan = SqlValue::Real(std::numeric_limits<double>::quiet_NaN());
  const SqlValue positive_infinity = SqlValue::Real(std::numeric_limits<double>::infinity());
  const SqlValue negative_infinity = SqlValue::Real(-std::numeric_limits<double>::infinity());
  const SqlValue negative_zero = SqlValue::Real(-0.0);

  ExpectNull(nan);
  ASSERT_TRUE(positive_infinity.real_value().has_value());
  EXPECT_TRUE(std::isinf(*positive_infinity.real_value()));
  EXPECT_FALSE(std::signbit(*positive_infinity.real_value()));
  ASSERT_TRUE(negative_infinity.real_value().has_value());
  EXPECT_TRUE(std::isinf(*negative_infinity.real_value()));
  EXPECT_TRUE(std::signbit(*negative_infinity.real_value()));
  ASSERT_TRUE(negative_zero.real_value().has_value());
  EXPECT_TRUE(std::signbit(*negative_zero.real_value()));
}

TEST(SqlAffinity, NoneAndBlobAffinityAreNoOps) {
  const SqlValue text = ApplyAffinity(TextValue("01"), TypeAffinity::kNone);
  const SqlValue blob = ApplyAffinity(BlobValue("123"), TypeAffinity::kBlob);
  const SqlValue integer = ApplyAffinity(SqlValue::Integer(7), TypeAffinity::kBlob);

  ExpectText(text, "01");
  ExpectBlob(blob, "123");
  ExpectInteger(integer, 7);
}

TEST(SqlAffinity, TextAffinityUsesSQLiteNumericRendering) {
  std::array cases{
      std::pair{SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                std::string_view{"-9223372036854775808"}},
      std::pair{SqlValue::Real(1.0), std::string_view{"1.0"}},
      std::pair{SqlValue::Real(-0.0), std::string_view{"0.0"}},
      std::pair{SqlValue::Real(1.0e20), std::string_view{"1.0e+20"}},
      std::pair{SqlValue::Real(1.0e-7), std::string_view{"1.0e-07"}},
      std::pair{SqlValue::Real(std::numeric_limits<double>::infinity()), std::string_view{"Inf"}},
      std::pair{SqlValue::Real(-std::numeric_limits<double>::infinity()), std::string_view{"-Inf"}},
  };

  for (auto& [input, expected] : cases) {
    ExpectText(ApplyAffinity(std::move(input), TypeAffinity::kText), expected);
  }
}

TEST(SqlAffinity, TextAffinityMatchesSQLiteSeventeenDigitRealRendering) {
  struct Case {
    std::uint64_t bits;
    std::string_view expected;
  };
  const std::array cases{
      Case{
          .bits = UINT64_C(0xbf40557610d02cbc),
          .expected = "-0.0004984690182267197",
      },
      Case{
          .bits = UINT64_C(0xf546144501033e6f),
          .expected = "-8.2879899888294192e+256",
      },
      Case{
          .bits = UINT64_C(0x433b08f5a323d719),
          .expected = "7609675468232473.0",
      },
      Case{
          .bits = UINT64_C(0x0000000000000001),
          .expected = "4.9406564584124654e-324",
      },
      Case{
          .bits = UINT64_C(0x7fefffffffffffff),
          .expected = "1.7976931348623157e+308",
      },
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.expected);
    ExpectText(
        ApplyAffinity(SqlValue::Real(std::bit_cast<double>(test_case.bits)), TypeAffinity::kText),
        test_case.expected);
  }
}

TEST(SqlAffinity, SQLiteRealRenderingRoundTripsFiniteBinary64Values) {
  std::uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (int iteration = 0; iteration < 4096; ++iteration) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    const std::uint64_t exponent = (state >> 52U) & UINT64_C(0x7ff);
    if (exponent == UINT64_C(0x7ff) || (state << 1U) == 0) {
      continue;
    }

    const SqlValue text =
        CastValue(SqlValue::Real(std::bit_cast<double>(state)), CastTarget::kText);
    const auto text_view = text.text_value();
    ASSERT_TRUE(text_view.has_value());
    const SqlValue parsed = CastValue(TextValue(text_view->bytes()), CastTarget::kReal);

    ASSERT_TRUE(parsed.real_value().has_value());
    EXPECT_EQ(state, std::bit_cast<std::uint64_t>(*parsed.real_value()))
        << "iteration " << iteration;
  }
}

TEST(SqlAffinity, NumericAndIntegerAffinityConvertCompleteDecimalSpellings) {
  struct Case {
    std::string_view input;
    SqlValueType type;
    std::int64_t integer;
    double real;
  };
  const std::array cases{
      Case{.input = "01", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = "1.0", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = "1e0", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = " 1 ", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{
          .input = "9223372036854775807",
          .type = SqlValueType::kInteger,
          .integer = std::numeric_limits<std::int64_t>::max(),
          .real = 0.0,
      },
      Case{
          .input = "-9223372036854775808",
          .type = SqlValueType::kInteger,
          .integer = std::numeric_limits<std::int64_t>::min(),
          .real = 0.0,
      },
      Case{
          .input = "9223372036854775808",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = 9223372036854775808.0,
      },
      Case{
          .input = "-9223372036854775809",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = -9223372036854775808.0,
      },
      Case{
          .input = "9000000000000000001.",
          .type = SqlValueType::kInteger,
          .integer = 9000000000000000000,
          .real = 0.0,
      },
      Case{
          .input = "1e309",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = std::numeric_limits<double>::infinity(),
      },
      Case{.input = "1e-400", .type = SqlValueType::kInteger, .integer = 0, .real = 0.0},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    const SqlValue numeric = ApplyAffinity(TextValue(test_case.input), TypeAffinity::kNumeric);
    const SqlValue integer = ApplyAffinity(TextValue(test_case.input), TypeAffinity::kInteger);
    for (const SqlValue* result : {&numeric, &integer}) {
      if (test_case.type == SqlValueType::kInteger) {
        ExpectInteger(*result, test_case.integer);
      } else {
        ExpectReal(*result, test_case.real);
      }
    }
  }
}

TEST(SqlAffinity, NumericAffinityLeavesIncompleteAndNonDecimalTextUnchanged) {
  for (const std::string_view input : {"1x", "", " ", "0x10", "+", "."}) {
    SCOPED_TRACE(input);
    ExpectText(ApplyAffinity(TextValue(input), TypeAffinity::kNumeric), input);
  }
  ExpectBlob(ApplyAffinity(BlobValue("123"), TypeAffinity::kNumeric), "123");
}

TEST(SqlAffinity, NumericAffinityRetainsNonNumericTextStorage) {
  SqlValue input = TextValue(std::string(1024, 'x'));
  const auto input_view = input.text_value();
  ASSERT_TRUE(input_view.has_value());
  const char* const original_data = input_view->data();

  const SqlValue result = ApplyAffinity(std::move(input), TypeAffinity::kNumeric);

  const auto result_view = result.text_value();
  ASSERT_TRUE(result_view.has_value());
  EXPECT_EQ(original_data, result_view->data());
}

TEST(SqlAffinity, RealAffinityMaterializesRealValuesOnlyForNumericInputs) {
  ExpectReal(ApplyAffinity(SqlValue::Integer(7), TypeAffinity::kReal), 7.0);
  ExpectReal(ApplyAffinity(TextValue("1"), TypeAffinity::kReal), 1.0);
  ExpectReal(ApplyAffinity(TextValue("1.25"), TypeAffinity::kReal), 1.25);
  const SqlValue negative_zero = ApplyAffinity(TextValue("-0.0"), TypeAffinity::kReal);
  ASSERT_TRUE(negative_zero.real_value().has_value());
  EXPECT_FALSE(std::signbit(*negative_zero.real_value()));
  ExpectText(ApplyAffinity(TextValue("1x"), TypeAffinity::kReal), "1x");
  ExpectBlob(ApplyAffinity(BlobValue("1"), TypeAffinity::kReal), "1");
  ExpectNull(ApplyAffinity(SqlValue{}, TypeAffinity::kReal));
}

TEST(SqlCast, IntegerConsumesPrefixesTruncatesAndSaturates) {
  struct Case {
    std::string_view input;
    std::int64_t expected;
  };
  const std::array cases{
      Case{.input = "123abc", .expected = 123},
      Case{.input = "-2.12e-01ABC", .expected = -2},
      Case{.input = "abc", .expected = 0},
      Case{.input = "", .expected = 0},
      Case{.input = "0x1234", .expected = 0},
      Case{
          .input = "9223372036854775808",
          .expected = std::numeric_limits<std::int64_t>::max(),
      },
      Case{
          .input = "-9223372036854775809",
          .expected = std::numeric_limits<std::int64_t>::min(),
      },
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    ExpectInteger(CastValue(TextValue(test_case.input), CastTarget::kInteger), test_case.expected);
  }

  ExpectInteger(CastValue(SqlValue::Real(3.9), CastTarget::kInteger), 3);
  ExpectInteger(CastValue(SqlValue::Real(-3.9), CastTarget::kInteger), -3);
  ExpectInteger(
      CastValue(SqlValue::Real(std::numeric_limits<double>::infinity()), CastTarget::kInteger),
      std::numeric_limits<std::int64_t>::max());
  ExpectInteger(CastValue(BlobValue("42x"), CastTarget::kInteger), 42);
  ExpectNull(CastValue(SqlValue{}, CastTarget::kInteger));
}

TEST(SqlCast, RealConsumesDecimalPrefixesAndDefaultsToZero) {
  ExpectReal(CastValue(TextValue("123.5abc"), CastTarget::kReal), 123.5);
  ExpectReal(CastValue(TextValue("abc"), CastTarget::kReal), 0.0);
  ExpectReal(CastValue(TextValue("1e309"), CastTarget::kReal),
             std::numeric_limits<double>::infinity());
  ExpectReal(CastValue(SqlValue::Integer(42), CastTarget::kReal), 42.0);
  ExpectReal(CastValue(BlobValue("-2.5x"), CastTarget::kReal), -2.5);
  ExpectNull(CastValue(SqlValue{}, CastTarget::kReal));
}

TEST(SqlCast, RealUsesSQLiteDecimalToBinaryRounding) {
  struct Case {
    std::string_view input;
    std::uint64_t expected_bits;
  };
  const std::array cases{
      Case{
          .input = "803531074237660579555.423766057955561667934E105",
          .expected_bits = UINT64_C(0x5a12fe1b2d33a348),
      },
      Case{
          .input = "4.9406564584124654e-324",
          .expected_bits = UINT64_C(0x0000000000000001),
      },
      Case{.input = "2e-324", .expected_bits = UINT64_C(0x0000000000000000)},
      Case{.input = "3e-324", .expected_bits = UINT64_C(0x0000000000000001)},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    const SqlValue result = CastValue(TextValue(test_case.input), CastTarget::kReal);
    ASSERT_TRUE(result.real_value().has_value());
    EXPECT_EQ(test_case.expected_bits, std::bit_cast<std::uint64_t>(*result.real_value()));
  }
}

TEST(SqlCast, NumericUsesPrefixParsingAndSQLiteExactIntegerRange) {
  ExpectInteger(CastValue(TextValue("123abc"), CastTarget::kNumeric), 123);
  ExpectInteger(CastValue(TextValue("1.0"), CastTarget::kNumeric), 1);
  ExpectInteger(CastValue(TextValue("9.223372036e14"), CastTarget::kNumeric), 922337203600000);
  ExpectReal(CastValue(TextValue("9.223372036e15"), CastTarget::kNumeric), 9223372036000000.0);
  ExpectInteger(CastValue(TextValue("2251799813685247.0"), CastTarget::kNumeric), 2251799813685247);
  ExpectReal(CastValue(TextValue("2251799813685248.0"), CastTarget::kNumeric), 2251799813685248.0);
  ExpectInteger(CastValue(TextValue("-2251799813685248.0"), CastTarget::kNumeric),
                -2251799813685248);
  ExpectInteger(CastValue(TextValue("abc"), CastTarget::kNumeric), 0);

  const SqlValue existing_real = CastValue(SqlValue::Real(13.0), CastTarget::kNumeric);
  ExpectReal(existing_real, 13.0);
}

TEST(SqlCast, TextAndBlobCastsUseCanonicalTextBytes) {
  ExpectText(CastValue(SqlValue::Integer(123), CastTarget::kText), "123");
  ExpectText(CastValue(SqlValue::Real(1.0), CastTarget::kText), "1.0");
  ExpectText(CastValue(SqlValue::Real(-0.0), CastTarget::kText), "0.0");
  ExpectText(CastValue(BlobValue(std::string{"a\0b", 3}), CastTarget::kText),
             std::string{"a\0b", 3});
  ExpectBlob(CastValue(TextValue(std::string{"a\0b", 3}), CastTarget::kBlob),
             std::string{"a\0b", 3});
  ExpectBlob(CastValue(SqlValue::Real(1.0), CastTarget::kBlob), "1.0");
  ExpectNull(CastValue(SqlValue{}, CastTarget::kText));
  ExpectNull(CastValue(SqlValue{}, CastTarget::kBlob));
}

TEST(SqlValueComparison, OrdersSQLiteStorageClassesAndBinaryPayloads) {
  const SqlValue null_value;
  const SqlValue number = SqlValue::Integer(1);
  const SqlValue text = TextValue("1");
  const SqlValue blob = BlobValue("1");

  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(null_value, number));
  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(number, text));
  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(text, blob));
  EXPECT_EQ(std::strong_ordering::greater, CompareSqlValues(blob, null_value));

  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(TextValue("a"), TextValue("aa")));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(TextValue(std::string{"a\0", 2}), TextValue(std::string{"a\1", 2})));
  EXPECT_EQ(std::strong_ordering::greater,
            CompareSqlValues(BlobValue(std::string{"\xff", 1}), BlobValue(std::string{"\x7f", 1})));
}

TEST(SqlValueComparison, ComparesIntegerAndRealWithoutLosingIntegerPrecision) {
  EXPECT_EQ(std::strong_ordering::greater, CompareSqlValues(SqlValue::Integer(9007199254740993LL),
                                                            SqlValue::Real(9007199254740992.0)));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
                             SqlValue::Real(9223372036854775808.0)));
  EXPECT_EQ(std::strong_ordering::equal,
            CompareSqlValues(SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                             SqlValue::Real(-9223372036854775808.0)));
  EXPECT_EQ(std::strong_ordering::equal,
            CompareSqlValues(SqlValue::Integer(0), SqlValue::Real(-0.0)));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(SqlValue::Integer(0),
                             SqlValue::Real(std::numeric_limits<double>::infinity())));
  EXPECT_EQ(std::strong_ordering::greater,
            CompareSqlValues(SqlValue::Integer(0),
                             SqlValue::Real(-std::numeric_limits<double>::infinity())));
}

TEST(SqlComparison, PropagatesNullForOrdinaryOperators) {
  for (const SqlComparison comparison :
       {SqlComparison::kEqual, SqlComparison::kNotEqual, SqlComparison::kLess,
        SqlComparison::kLessEqual, SqlComparison::kGreater, SqlComparison::kGreaterEqual}) {
    EXPECT_EQ(SqlTruthValue::kNull,
              EvaluateSqlComparison(SqlValue{}, SqlValue::Integer(1), comparison));
    EXPECT_EQ(SqlTruthValue::kNull,
              EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, comparison));
  }
  EXPECT_EQ(SqlTruthValue::kTrue, EvaluateSqlComparison(SqlValue::Integer(1), SqlValue::Real(1.0),
                                                        SqlComparison::kEqual));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), TextValue("1"), SqlComparison::kEqual));
}

TEST(SqlComparison, IsAndIsNotCompareNullAsAValue) {
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue{}, SqlValue{}, SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue{}, SqlValue{}, SqlComparison::kIsNot));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, SqlComparison::kIsNot));
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue::Real(1.0), SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), TextValue("1"), SqlComparison::kIs));
}

}  // namespace
}  // namespace modern_sqlite
