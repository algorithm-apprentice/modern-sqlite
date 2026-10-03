#include "modern_sqlite/text/text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace modern_sqlite {
namespace {

static_assert(std::is_trivially_copyable_v<Utf8View>);
static_assert(std::is_trivially_copyable_v<SourceSpan>);
static_assert(sizeof(Utf8View) == sizeof(std::string_view));
static_assert(sizeof(SourceSpan) == 2 * sizeof(std::size_t));

[[nodiscard]] std::string ByteString(std::initializer_list<std::uint8_t> bytes) {
  std::string result;
  result.reserve(bytes.size());
  for (const std::uint8_t byte : bytes) {
    result.push_back(std::bit_cast<char>(byte));
  }
  return result;
}

void ExpectDecoded(std::initializer_list<std::uint8_t> bytes, char32_t expected_code_point) {
  const std::string text = ByteString(bytes);

  const auto decoded = DecodeUtf8(Utf8View{text}, ByteOffset{0});

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(expected_code_point, decoded->code_point);
  EXPECT_EQ(text.size(), decoded->byte_count.value());
}

[[nodiscard]] constexpr Utf8Error ExpectedUtf8Error(Utf8ErrorKind kind,
                                                    std::size_t offset) noexcept {
  return Utf8Error{.kind = kind, .offset = ByteOffset{offset}};
}

TEST(Utf8View, BorrowsAndPreservesEmbeddedNullAndMalformedBytes) {
  const std::string text = ByteString({'a', 0x00, 0x80, 'z'});
  const Utf8View view{text};

  EXPECT_EQ(text.data(), view.data());
  EXPECT_EQ(text.size(), view.size_bytes());
  EXPECT_EQ(std::string_view{text}, view.bytes());
}

TEST(SourceSpan, CreatesAndSlicesHalfOpenByteRangesWithoutCopying) {
  const std::string text = "hello";
  const Utf8View view{text};
  const auto span = SourceSpan::FromBounds(ByteOffset{1}, ByteOffset{4});

  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(1U, span->begin().value());
  EXPECT_EQ(4U, span->end().value());
  EXPECT_EQ(3U, span->length().value());
  EXPECT_FALSE(span->empty());

  const auto subview = Slice(view, *span);
  ASSERT_TRUE(subview.has_value());
  EXPECT_EQ("ell", subview->bytes());
  EXPECT_EQ(text.data() + 1, subview->data());
}

TEST(SourceSpan, CreatesOffsetLengthAndEmptySuffixRanges) {
  const auto suffix = SourceSpan::FromOffsetAndLength(ByteOffset{2}, ByteCount{3});
  const auto empty = SourceSpan::FromOffsetAndLength(ByteOffset{5}, ByteCount{0});

  ASSERT_TRUE(suffix.has_value());
  EXPECT_EQ(2U, suffix->begin().value());
  EXPECT_EQ(5U, suffix->end().value());
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());

  const auto suffix_view = Slice(Utf8View{"hello"}, *suffix);
  const auto empty_view = Slice(Utf8View{"hello"}, *empty);
  ASSERT_TRUE(suffix_view.has_value());
  EXPECT_EQ("llo", suffix_view->bytes());
  ASSERT_TRUE(empty_view.has_value());
  EXPECT_TRUE(empty_view->empty());
}

TEST(SourceSpan, RejectsReversedOverflowingAndOutOfRangeSpans) {
  const auto reversed = SourceSpan::FromBounds(ByteOffset{4}, ByteOffset{3});
  const auto overflowing = SourceSpan::FromOffsetAndLength(
      ByteOffset{std::numeric_limits<std::size_t>::max()}, ByteCount{1});
  const auto long_span = SourceSpan::FromBounds(ByteOffset{1}, ByteOffset{6});

  ASSERT_FALSE(reversed.has_value());
  EXPECT_EQ(SourceSpanError::kReversedBounds, reversed.error());
  ASSERT_FALSE(overflowing.has_value());
  EXPECT_EQ(SourceSpanError::kOffsetOverflow, overflowing.error());
  ASSERT_TRUE(long_span.has_value());

  const auto out_of_range = Slice(Utf8View{"hello"}, *long_span);
  ASSERT_FALSE(out_of_range.has_value());
  EXPECT_EQ(SourceSpanError::kOutOfRange, out_of_range.error());
}

TEST(Utf8Decode, AcceptsEveryUnicodeEncodingBoundary) {
  ExpectDecoded({0x00}, U'\0');
  ExpectDecoded({0x7f}, U'\x7f');
  ExpectDecoded({0xc2, 0x80}, U'\u0080');
  ExpectDecoded({0xdf, 0xbf}, U'\u07ff');
  ExpectDecoded({0xe0, 0xa0, 0x80}, U'\u0800');
  ExpectDecoded({0xed, 0x9f, 0xbf}, U'\ud7ff');
  ExpectDecoded({0xee, 0x80, 0x80}, U'\ue000');
  ExpectDecoded({0xef, 0xbf, 0xbf}, U'\uffff');
  ExpectDecoded({0xf0, 0x90, 0x80, 0x80}, U'\U00010000');
  ExpectDecoded({0xf4, 0x8f, 0xbf, 0xbf}, U'\U0010ffff');
}

TEST(Utf8Decode, DecodesAtByteOffsetsAndCountsCodePoints) {
  const std::string text = ByteString({'A', 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80});
  const Utf8View view{text};

  const auto euro = DecodeUtf8(view, ByteOffset{1});
  const auto face = DecodeUtf8(view, ByteOffset{4});
  const auto count = CountUtf8CodePoints(view);

  ASSERT_TRUE(euro.has_value());
  EXPECT_EQ(U'\u20ac', euro->code_point);
  EXPECT_EQ(3U, euro->byte_count.value());
  ASSERT_TRUE(face.has_value());
  EXPECT_EQ(U'\U0001f600', face->code_point);
  EXPECT_EQ(4U, face->byte_count.value());
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(3U, *count);
  EXPECT_TRUE(ValidateUtf8(view).has_value());
}

TEST(Utf8Decode, UsesExplicitViewLengthIncludingNullBytes) {
  const std::string text = ByteString({'a', 0x00, 'b'});
  const Utf8View view{text};

  const auto null_code_point = DecodeUtf8(view, ByteOffset{1});
  const auto count = CountUtf8CodePoints(view);
  const auto empty_count = CountUtf8CodePoints(Utf8View{});

  ASSERT_TRUE(null_code_point.has_value());
  EXPECT_EQ(U'\0', null_code_point->code_point);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(3U, *count);
  ASSERT_TRUE(empty_count.has_value());
  EXPECT_EQ(0U, *empty_count);
}

TEST(Utf8Decode, ReportsEndAndOutOfRangeOffsets) {
  const auto at_end = DecodeUtf8(Utf8View{"a"}, ByteOffset{1});
  const auto past_end = DecodeUtf8(Utf8View{"a"}, ByteOffset{2});

  ASSERT_FALSE(at_end.has_value());
  EXPECT_EQ(ExpectedUtf8Error(Utf8ErrorKind::kEndOfInput, 1), at_end.error());
  ASSERT_FALSE(past_end.has_value());
  EXPECT_EQ(ExpectedUtf8Error(Utf8ErrorKind::kOffsetOutOfRange, 2), past_end.error());
}

TEST(Utf8Decode, RejectsMalformedSequencesAtTheFirstFailingByte) {
  struct Case {
    std::string text;
    Utf8Error expected;
  };
  const std::array cases{
      Case{
          .text = ByteString({0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kUnexpectedContinuationByte, 0),
      },
      Case{
          .text = ByteString({0xc0, 0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kOverlongEncoding, 0),
      },
      Case{
          .text = ByteString({0xe0, 0x80, 0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kOverlongEncoding, 0),
      },
      Case{
          .text = ByteString({0xe2, 0x28, 0xa1}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kInvalidContinuationByte, 1),
      },
      Case{
          .text = ByteString({0xe2, 0x82}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kTruncatedSequence, 2),
      },
      Case{
          .text = ByteString({0xed, 0xa0, 0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kSurrogateCodePoint, 0),
      },
      Case{
          .text = ByteString({0xf4, 0x90, 0x80, 0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kCodePointOutOfRange, 0),
      },
      Case{
          .text = ByteString({0xf5, 0x80, 0x80, 0x80}),
          .expected = ExpectedUtf8Error(Utf8ErrorKind::kCodePointOutOfRange, 0),
      },
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(testing::PrintToString(test_case.text));
    const auto decoded = DecodeUtf8(Utf8View{test_case.text}, ByteOffset{0});
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(test_case.expected, decoded.error());
  }
}

TEST(Utf8Decode, ValidationReportsErrorsWithoutChangingRawBytes) {
  const std::string text = ByteString({'a', 0xe2, 0x28, 0xa1, 'z'});
  const Utf8View view{text};

  const auto validation = ValidateUtf8(view);
  const auto count = CountUtf8CodePoints(view);

  ASSERT_FALSE(validation.has_value());
  EXPECT_EQ(ExpectedUtf8Error(Utf8ErrorKind::kInvalidContinuationByte, 2), validation.error());
  ASSERT_FALSE(count.has_value());
  EXPECT_EQ(validation.error(), count.error());
  EXPECT_EQ(std::string_view{text}, view.bytes());
}

TEST(SqliteCharacterClasses, MatchPinnedSqliteTableForEveryByte) {
  constexpr std::array<std::uint8_t, 256> kSqliteCharacterClasses{
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02, 0x02, 0x02, 0x02,
      0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
      0x02, 0x80, 0x00, 0x00, 0x00, 0x40, 0x80, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22, 0x22,
      0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
      0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
      0x40,
  };

  for (std::size_t value = 0; value < kSqliteCharacterClasses.size(); ++value) {
    SCOPED_TRACE(value);
    const auto byte = static_cast<std::uint8_t>(value);
    const std::uint8_t flags = kSqliteCharacterClasses[value];

    EXPECT_EQ((flags & 0x01U) != 0U, IsSqliteSpace(byte));
    EXPECT_EQ((flags & 0x02U) != 0U, IsSqliteAlpha(byte));
    EXPECT_EQ((flags & 0x04U) != 0U, IsSqliteDigit(byte));
    EXPECT_EQ((flags & 0x06U) != 0U, IsSqliteAlnum(byte));
    EXPECT_EQ((flags & 0x08U) != 0U, IsSqliteHexDigit(byte));
    EXPECT_EQ((flags & 0x46U) != 0U, IsSqliteIdentifierByte(byte));
    EXPECT_EQ((flags & 0x80U) != 0U, IsSqliteQuote(byte));
  }
}

TEST(SqliteCase, MatchesPinnedAsciiMapsForEveryByte) {
  for (std::size_t value = 0; value <= std::numeric_limits<std::uint8_t>::max(); ++value) {
    SCOPED_TRACE(value);
    const auto byte = static_cast<std::uint8_t>(value);
    const auto expected_lower = static_cast<std::uint8_t>(
        byte >= static_cast<std::uint8_t>('A') && byte <= static_cast<std::uint8_t>('Z')
            ? byte + static_cast<std::uint8_t>('a' - 'A')
            : byte);
    const auto expected_upper = static_cast<std::uint8_t>(
        byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('z')
            ? byte - static_cast<std::uint8_t>('a' - 'A')
            : byte);

    EXPECT_EQ(expected_lower, SqliteToLower(byte));
    EXPECT_EQ(expected_upper, SqliteToUpper(byte));
  }
}

}  // namespace
}  // namespace modern_sqlite
