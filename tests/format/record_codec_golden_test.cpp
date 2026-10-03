#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] constexpr std::uint8_t HexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  return static_cast<std::uint8_t>(value - 'A' + 10);
}

[[nodiscard]] ByteBuffer FromHex(std::string_view hex) {
  ByteBuffer bytes{ByteCount{hex.size() / 2U}};
  for (std::size_t offset = 0; offset < bytes.size().value(); ++offset) {
    const std::uint8_t high = HexDigit(hex[offset * 2U]);
    const std::uint8_t low = HexDigit(hex[(offset * 2U) + 1U]);
    const auto combined =
        (static_cast<std::uint32_t>(high) << 4U) | static_cast<std::uint32_t>(low);
    bytes.mutable_view()[offset] = static_cast<std::byte>(combined);
  }
  return bytes;
}

[[nodiscard]] ByteBuffer Blob(std::initializer_list<std::uint8_t> values) {
  ByteBuffer result{ByteCount{values.size()}};
  std::size_t offset = 0;
  for (const std::uint8_t value : values) {
    result.mutable_view()[offset] = static_cast<std::byte>(value);
    ++offset;
  }
  return result;
}

TEST(RecordCodecGolden, MatchesSQLite354AllStorageClassesAndIntegerWidths) {
  // Extracted from a table-leaf payload written by pinned SQLite 3.54.0.
  const ByteBuffer expected = FromHex(
      "150008090102020203030404050506060715140d0c"
      "7f0080ff7f7fff0080007fffff008000007fffffff"
      "0000800000007fffffffffff000080000000000080"
      "00000000000000400a000000000000410080ff0001feff");

  std::vector<SqlValue> values;
  values.emplace_back();
  values.push_back(SqlValue::Integer(0));
  values.push_back(SqlValue::Integer(1));
  values.push_back(SqlValue::Integer(127));
  values.push_back(SqlValue::Integer(128));
  values.push_back(SqlValue::Integer(-129));
  values.push_back(SqlValue::Integer(32767));
  values.push_back(SqlValue::Integer(32768));
  values.push_back(SqlValue::Integer(8388607));
  values.push_back(SqlValue::Integer(8388608));
  values.push_back(SqlValue::Integer(2147483647));
  values.push_back(SqlValue::Integer(2147483648LL));
  values.push_back(SqlValue::Integer(140737488355327LL));
  values.push_back(SqlValue::Integer(140737488355328LL));
  values.push_back(SqlValue::Integer(std::numeric_limits<std::int64_t>::min()));
  values.push_back(SqlValue::Real(3.25));
  values.push_back(SqlValue::Text(std::string{"A\0\x80\xff", 4}));
  values.push_back(SqlValue::Blob(Blob({0x00, 0x01, 0xfe, 0xff})));
  values.push_back(SqlValue::Text(""));
  values.push_back(SqlValue::Blob(ByteBuffer{}));

  const auto encoded = EncodeRecord(values);

  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(std::ranges::equal(expected.view(), encoded->view()));

  const auto decoded = DecodeRecord(expected.view());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(values.size(), decoded->size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_EQ(std::strong_ordering::equal, CompareSqlValues(values[index], (*decoded)[index]))
        << index;
    EXPECT_EQ(values[index].type(), (*decoded)[index].type()) << index;
  }
}

TEST(RecordCodecGolden, MatchesSQLite354SchemaFormatOneAndFourConstants) {
  // Both vectors were extracted from SQLite-created table-leaf payloads.
  const ByteBuffer legacy = FromHex("0301010001");
  const ByteBuffer current = FromHex("030809");
  std::vector<SqlValue> values;
  values.push_back(SqlValue::Integer(0));
  values.push_back(SqlValue::Integer(1));
  const RecordCodecOptions legacy_options{
      .schema_format = RecordSchemaFormat::kOne,
  };

  const auto legacy_encoded = EncodeRecord(values, legacy_options);
  const auto current_encoded = EncodeRecord(values);

  ASSERT_TRUE(legacy_encoded.has_value());
  ASSERT_TRUE(current_encoded.has_value());
  EXPECT_TRUE(std::ranges::equal(legacy.view(), legacy_encoded->view()));
  EXPECT_TRUE(std::ranges::equal(current.view(), current_encoded->view()));
}

TEST(RecordCodecGolden, MatchesSQLite354RealBitPatterns) {
  // SQLite input: CAST('-0.0' AS REAL), 3.25, and positive infinity.
  const ByteBuffer expected = FromHex("040707078000000000000000400a0000000000007ff0000000000000");
  std::vector<SqlValue> values;
  values.push_back(SqlValue::Real(-0.0));
  values.push_back(SqlValue::Real(3.25));
  values.push_back(SqlValue::Real(std::numeric_limits<double>::infinity()));

  const auto encoded = EncodeRecord(values);

  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(std::ranges::equal(expected.view(), encoded->view()));
  const auto decoded = DecodeRecord(expected.view());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(3U, decoded->size());
  ASSERT_TRUE((*decoded)[0].real_value().has_value());
  EXPECT_TRUE(std::signbit((*decoded)[0].real_value().value_or(0.0)));
  EXPECT_EQ(3.25, (*decoded)[1].real_value());
  EXPECT_EQ(std::numeric_limits<double>::infinity(), (*decoded)[2].real_value());
}

TEST(RecordCodecGolden, MatchesSQLite354SerialTypeVarintBoundaries) {
  // SQLite produced header codes 127, 129, 126, and 128 for lengths 57/58.
  std::vector<SqlValue> values;
  values.push_back(SqlValue::Text(std::string(57, 'a')));
  values.push_back(SqlValue::Text(std::string(58, 'b')));
  values.push_back(SqlValue::Blob(ByteBuffer{ByteCount{57}}));
  values.push_back(SqlValue::Blob(ByteBuffer{ByteCount{58}}));

  const auto encoded = EncodeRecord(values);

  ASSERT_TRUE(encoded.has_value());
  ASSERT_EQ(237U, encoded->size().value());
  const ByteBuffer expected_header = FromHex("077f81017e8100");
  EXPECT_TRUE(std::ranges::equal(expected_header.view(), encoded->view().first(7)));
  const auto decoded = DecodeRecord(encoded->view());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(4U, decoded->size());
  EXPECT_EQ(57U, (*decoded)[0].text_value().value_or(Utf8View{}).size_bytes());
  EXPECT_EQ(58U, (*decoded)[1].text_value().value_or(Utf8View{}).size_bytes());
  EXPECT_EQ(57U, (*decoded)[2].blob_value().value_or(ByteView{}).size());
  EXPECT_EQ(58U, (*decoded)[3].blob_value().value_or(ByteView{}).size());
}

}  // namespace
}  // namespace modern_sqlite
