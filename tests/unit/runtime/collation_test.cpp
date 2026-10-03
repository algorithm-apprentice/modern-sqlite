#include "modern_sqlite/runtime/collation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/text/text.hpp"

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

struct OracleValue {
  int id;
  std::string text;
};

[[nodiscard]] std::vector<OracleValue> OracleCorpus() {
  return {
      {.id = 0, .text = TextFromBytes({})},
      {.id = 1, .text = TextFromBytes({0x20})},
      {.id = 2, .text = TextFromBytes({0x20, 0x20})},
      {.id = 3, .text = TextFromBytes({0x09})},
      {.id = 4, .text = TextFromBytes({0x41})},
      {.id = 5, .text = TextFromBytes({0x61})},
      {.id = 6, .text = TextFromBytes({0x42})},
      {.id = 7, .text = TextFromBytes({0x62})},
      {.id = 8, .text = TextFromBytes({0xc3, 0x84})},
      {.id = 9, .text = TextFromBytes({0xc3, 0xa4})},
      {.id = 10, .text = TextFromBytes({0x61, 0x00, 0x62})},
      {.id = 11, .text = TextFromBytes({0x41, 0x00, 0x63})},
      {.id = 12, .text = TextFromBytes({0x61, 0x00})},
      {.id = 13, .text = TextFromBytes({0x61, 0x20})},
      {.id = 14, .text = TextFromBytes({0x61, 0xc2, 0xa0})},
      {.id = 15, .text = TextFromBytes({0x00})},
      {.id = 16, .text = TextFromBytes({0x00, 0x00})},
      {.id = 17, .text = TextFromBytes({0xff})},
  };
}

[[nodiscard]] std::vector<int> SortedOracleIds(const Collation& collation) {
  std::vector<OracleValue> values = OracleCorpus();
  std::ranges::sort(values, [&collation](const OracleValue& left, const OracleValue& right) {
    const std::weak_ordering ordering =
        collation.Compare(Utf8View{left.text}, Utf8View{right.text});
    if (ordering == std::weak_ordering::equivalent) {
      return left.id < right.id;
    }
    return ordering == std::weak_ordering::less;
  });

  std::vector<int> ids;
  ids.reserve(values.size());
  for (const OracleValue& value : values) {
    ids.push_back(value.id);
  }
  return ids;
}

class ReverseCollation final : public Collation {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "REVERSE"; }

  [[nodiscard]] std::weak_ordering Compare(Utf8View left, Utf8View right) const noexcept override {
    const std::weak_ordering ordering = BinaryCollation().Compare(left, right);
    if (ordering == std::weak_ordering::less) {
      return std::weak_ordering::greater;
    }
    if (ordering == std::weak_ordering::greater) {
      return std::weak_ordering::less;
    }
    return std::weak_ordering::equivalent;
  }
};

static_assert(!std::is_copy_constructible_v<Collation>);
static_assert(!std::is_move_constructible_v<Collation>);
static_assert(!std::is_copy_constructible_v<ReverseCollation>);
static_assert(!std::is_move_constructible_v<ReverseCollation>);

TEST(CollationTest, ExposesStableBuiltInIdentities) {
  EXPECT_EQ(BinaryCollation().name(), "BINARY");
  EXPECT_EQ(NoCaseCollation().name(), "NOCASE");
  EXPECT_EQ(RTrimCollation().name(), "RTRIM");

  EXPECT_EQ(&BinaryCollation(), &BinaryCollation());
  EXPECT_EQ(&NoCaseCollation(), &NoCaseCollation());
  EXPECT_EQ(&RTrimCollation(), &RTrimCollation());
}

TEST(CollationTest, BinaryUsesUnsignedByteAndPrefixOrdering) {
  for (unsigned left = 0; left < 256; ++left) {
    for (unsigned right = 0; right < 256; ++right) {
      const std::string left_text = TextFromBytes({static_cast<std::uint8_t>(left)});
      const std::string right_text = TextFromBytes({static_cast<std::uint8_t>(right)});
      const std::weak_ordering expected = left < right   ? std::weak_ordering::less
                                          : left > right ? std::weak_ordering::greater
                                                         : std::weak_ordering::equivalent;
      EXPECT_EQ(BinaryCollation().Compare(Utf8View{left_text}, Utf8View{right_text}), expected);
    }
  }

  const std::string prefix = TextFromBytes({0x61, 0x00});
  const std::string longer = TextFromBytes({0x61, 0x00, 0xff});
  EXPECT_EQ(BinaryCollation().Compare(Utf8View{prefix}, Utf8View{longer}),
            std::weak_ordering::less);
  EXPECT_EQ(BinaryCollation().Compare(Utf8View{longer}, Utf8View{prefix}),
            std::weak_ordering::greater);
}

TEST(CollationTest, NoCaseFoldsOnlyAsciiAndPreservesEmbeddedNullBehavior) {
  for (std::uint8_t offset = 0; offset < 26; ++offset) {
    const std::string upper = TextFromBytes({static_cast<std::uint8_t>('A' + offset)});
    const std::string lower = TextFromBytes({static_cast<std::uint8_t>('a' + offset)});
    EXPECT_EQ(NoCaseCollation().Compare(Utf8View{upper}, Utf8View{lower}),
              std::weak_ordering::equivalent);
  }

  const std::string upper_a_umlaut = TextFromBytes({0xc3, 0x84});
  const std::string lower_a_umlaut = TextFromBytes({0xc3, 0xa4});
  EXPECT_EQ(NoCaseCollation().Compare(Utf8View{upper_a_umlaut}, Utf8View{lower_a_umlaut}),
            std::weak_ordering::less);

  const std::string left = TextFromBytes({0x61, 0x00, 0x62});
  const std::string same_length = TextFromBytes({0x41, 0x00, 0x63});
  const std::string shorter = TextFromBytes({0x41, 0x00});
  EXPECT_EQ(NoCaseCollation().Compare(Utf8View{left}, Utf8View{same_length}),
            std::weak_ordering::equivalent);
  EXPECT_EQ(NoCaseCollation().Compare(Utf8View{left}, Utf8View{shorter}),
            std::weak_ordering::greater);
}

TEST(CollationTest, RTrimRemovesOnlyTrailingAsciiSpaces) {
  const std::string plain = TextFromBytes({0x61});
  const std::string spaces = TextFromBytes({0x61, 0x20, 0x20});
  const std::string tab = TextFromBytes({0x61, 0x09});
  const std::string non_breaking_space = TextFromBytes({0x61, 0xc2, 0xa0});
  const std::string embedded_null_and_space = TextFromBytes({0x61, 0x00, 0x20});
  const std::string embedded_null = TextFromBytes({0x61, 0x00});

  EXPECT_EQ(RTrimCollation().Compare(Utf8View{plain}, Utf8View{spaces}),
            std::weak_ordering::equivalent);
  EXPECT_EQ(RTrimCollation().Compare(Utf8View{plain}, Utf8View{tab}), std::weak_ordering::less);
  EXPECT_EQ(RTrimCollation().Compare(Utf8View{plain}, Utf8View{non_breaking_space}),
            std::weak_ordering::less);
  EXPECT_EQ(RTrimCollation().Compare(Utf8View{embedded_null_and_space}, Utf8View{embedded_null}),
            std::weak_ordering::equivalent);
}

TEST(CollationTest, MatchesPinnedSqliteOrderingCorpus) {
  EXPECT_EQ(SortedOracleIds(BinaryCollation()),
            (std::vector<int>{0, 15, 16, 3, 1, 2, 4, 11, 6, 5, 12, 10, 13, 14, 7, 8, 9, 17}));
  EXPECT_EQ(SortedOracleIds(NoCaseCollation()),
            (std::vector<int>{0, 15, 16, 3, 1, 2, 4, 5, 12, 10, 11, 13, 14, 6, 7, 8, 9, 17}));
  EXPECT_EQ(SortedOracleIds(RTrimCollation()),
            (std::vector<int>{0, 1, 2, 15, 16, 3, 4, 11, 6, 5, 13, 12, 10, 14, 7, 8, 9, 17}));
}

TEST(CollationTest, SupportsCustomOrderingContracts) {
  const ReverseCollation reverse;

  EXPECT_EQ(reverse.name(), "REVERSE");
  EXPECT_EQ(reverse.Compare(Utf8View{"alpha"}, Utf8View{"beta"}), std::weak_ordering::greater);
  EXPECT_EQ(reverse.Compare(Utf8View{"beta"}, Utf8View{"alpha"}), std::weak_ordering::less);
  EXPECT_EQ(reverse.Compare(Utf8View{"same"}, Utf8View{"same"}), std::weak_ordering::equivalent);
}

TEST(CollationTest, AppliesCollationOnlyWhenBothSqlValuesAreText) {
  const SqlValue lowercase = SqlValue::Text("alpha");
  const SqlValue uppercase = SqlValue::Text("ALPHA");
  EXPECT_EQ(CompareSqlValues(lowercase, uppercase, BinaryCollation()), std::weak_ordering::greater);
  EXPECT_EQ(CompareSqlValues(lowercase, uppercase, NoCaseCollation()),
            std::weak_ordering::equivalent);

  EXPECT_EQ(CompareSqlValues(SqlValue::Integer(1), SqlValue::Real(2.0), NoCaseCollation()),
            std::weak_ordering::less);
  EXPECT_EQ(CompareSqlValues(SqlValue::Text("z"), SqlValue::Blob(ByteBuffer::CopyOf(AsBytes("a"))),
                             ReverseCollation()),
            std::weak_ordering::less);
}

TEST(CollationTest, PreservesSqlNullAndIdentitySemantics) {
  const SqlValue left = SqlValue::Text("value");
  const SqlValue right = SqlValue::Text("VALUE");
  const SqlValue null;

  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kEqual, NoCaseCollation()),
            SqlTruthValue::kTrue);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kNotEqual, NoCaseCollation()),
            SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kLess, NoCaseCollation()),
            SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kLessEqual, NoCaseCollation()),
            SqlTruthValue::kTrue);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kGreater, NoCaseCollation()),
            SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kGreaterEqual, NoCaseCollation()),
            SqlTruthValue::kTrue);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kIs, NoCaseCollation()),
            SqlTruthValue::kTrue);
  EXPECT_EQ(EvaluateSqlComparison(left, right, SqlComparison::kIsNot, NoCaseCollation()),
            SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlComparison(left, null, SqlComparison::kEqual, NoCaseCollation()),
            SqlTruthValue::kNull);
  EXPECT_EQ(EvaluateSqlComparison(left, null, SqlComparison::kIs, NoCaseCollation()),
            SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlComparison(null, null, SqlComparison::kIs, NoCaseCollation()),
            SqlTruthValue::kTrue);
}

}  // namespace
}  // namespace modern_sqlite
