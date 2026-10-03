#include "modern_sqlite/base/coding.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace modern_sqlite {
namespace {

constexpr std::byte Byte(std::uint8_t value) noexcept { return static_cast<std::byte>(value); }

static_assert(SqliteVarintLength(0) == ByteCount{1});
static_assert(SqliteVarintLength(std::uint64_t{1} << 56U) == ByteCount{9});

TEST(CodingEndian, LoadsUnalignedBigEndianIntegers) {
  const std::array<std::byte, 17> bytes{
      Byte(0xaa), Byte(0x12), Byte(0x34), Byte(0x89), Byte(0xab), Byte(0xcd),
      Byte(0xef), Byte(0x01), Byte(0x23), Byte(0x45), Byte(0x67), Byte(0x89),
      Byte(0xab), Byte(0xcd), Byte(0xef), Byte(0xbb), Byte(0xcc),
  };
  const std::span<const std::byte> view{bytes};

  EXPECT_EQ(0x1234U, LoadBigEndian<std::uint16_t>(view.subspan<1, 2>()));
  EXPECT_EQ(0x89abcdefU, LoadBigEndian<std::uint32_t>(view.subspan<3, 4>()));
  EXPECT_EQ(0x0123456789abcdefULL, LoadBigEndian<std::uint64_t>(view.subspan<7, 8>()));
}

TEST(CodingEndian, StoresBigEndianIntegersWithoutTouchingAdjacentBytes) {
  std::array<std::byte, 17> bytes;
  bytes.fill(Byte(0xaa));
  const std::span<std::byte> view{bytes};

  StoreBigEndian<std::uint16_t>(view.subspan<1, 2>(), 0x1234U);
  StoreBigEndian<std::uint32_t>(view.subspan<4, 4>(), 0x89abcdefU);
  StoreBigEndian<std::uint64_t>(view.subspan<8, 8>(), 0x0123456789abcdefULL);

  const std::array<std::byte, 17> expected{
      Byte(0xaa), Byte(0x12), Byte(0x34), Byte(0xaa), Byte(0x89), Byte(0xab),
      Byte(0xcd), Byte(0xef), Byte(0x01), Byte(0x23), Byte(0x45), Byte(0x67),
      Byte(0x89), Byte(0xab), Byte(0xcd), Byte(0xef), Byte(0xaa),
  };
  EXPECT_EQ(expected, bytes);
}

TEST(CodingEndian, CheckedReadsRejectTruncatedInput) {
  const std::array<std::byte, 7> bytes{
      Byte(0x01), Byte(0x23), Byte(0x45), Byte(0x67), Byte(0x89), Byte(0xab), Byte(0xcd),
  };

  const auto sixteen = ReadBigEndian<std::uint16_t>(ByteView{bytes}.first(1));
  const auto thirty_two = ReadBigEndian<std::uint32_t>(ByteView{bytes}.first(3));
  const auto sixty_four = ReadBigEndian<std::uint64_t>(ByteView{bytes});

  ASSERT_FALSE(sixteen.has_value());
  ASSERT_FALSE(thirty_two.has_value());
  ASSERT_FALSE(sixty_four.has_value());
  EXPECT_EQ(CodingError::kInputTooShort, sixteen.error());
  EXPECT_EQ(CodingError::kInputTooShort, thirty_two.error());
  EXPECT_EQ(CodingError::kInputTooShort, sixty_four.error());
}

TEST(CodingEndian, CheckedWritesRejectSmallDestinationWithoutMutation) {
  std::array<std::byte, 7> bytes;
  bytes.fill(Byte(0xaa));

  const auto result = WriteBigEndian<std::uint64_t>(MutableByteView{bytes}, 0x0102030405060708ULL);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(CodingError::kOutputTooSmall, result.error());
  EXPECT_TRUE(std::ranges::all_of(bytes, [](std::byte value) { return value == Byte(0xaa); }));
}

TEST(CodingEndian, CheckedReadAndWriteRoundTripPrefixOnly) {
  std::array<std::byte, 10> bytes;
  bytes.fill(Byte(0xaa));

  const auto written = WriteBigEndian<std::uint64_t>(MutableByteView{bytes}, 0x0123456789abcdefULL);
  const auto read = ReadBigEndian<std::uint64_t>(ByteView{bytes});

  ASSERT_TRUE(written.has_value());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(0x0123456789abcdefULL, *read);
  EXPECT_EQ(Byte(0xaa), bytes[8]);
  EXPECT_EQ(Byte(0xaa), bytes[9]);
}

struct VarintVector {
  constexpr VarintVector(std::uint64_t input_value, std::array<std::byte, 9> input_encoded,
                         std::size_t input_size) noexcept
      : value(input_value), encoded(input_encoded), size(input_size) {}

  std::uint64_t value;
  std::array<std::byte, 9> encoded;
  std::size_t size;
};

constexpr std::array<VarintVector, 13> kVarintVectors{{
    {0, {Byte(0x00)}, 1},
    {1, {Byte(0x01)}, 1},
    {127, {Byte(0x7f)}, 1},
    {128, {Byte(0x81), Byte(0x00)}, 2},
    {16383, {Byte(0xff), Byte(0x7f)}, 2},
    {16384, {Byte(0x81), Byte(0x80), Byte(0x00)}, 3},
    {2097151, {Byte(0xff), Byte(0xff), Byte(0x7f)}, 3},
    {2097152, {Byte(0x81), Byte(0x80), Byte(0x80), Byte(0x00)}, 4},
    {72057594037927935ULL,
     {Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff),
      Byte(0x7f)},
     8},
    {72057594037927936ULL,
     {Byte(0x80), Byte(0xc0), Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80),
      Byte(0x80), Byte(0x00)},
     9},
    {9223372036854775807ULL,
     {Byte(0xbf), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff),
      Byte(0xff), Byte(0xff)},
     9},
    {9223372036854775808ULL,
     {Byte(0xc0), Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80),
      Byte(0x80), Byte(0x00)},
     9},
    {std::numeric_limits<std::uint64_t>::max(),
     {Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff), Byte(0xff),
      Byte(0xff), Byte(0xff)},
     9},
}};

TEST(CodingVarint, MatchesCanonicalSQLiteGoldenVectors) {
  for (const VarintVector& vector : kVarintVectors) {
    std::array<std::byte, 12> destination;
    destination.fill(Byte(0xaa));

    const auto encoded = EncodeSqliteVarint(vector.value, MutableByteView{destination});
    ASSERT_TRUE(encoded.has_value()) << vector.value;
    EXPECT_EQ(vector.size, encoded->value());
    EXPECT_EQ(vector.size, SqliteVarintLength(vector.value).value());
    EXPECT_TRUE(std::ranges::equal(ByteView{destination}.first(vector.size),
                                   ByteView{vector.encoded}.first(vector.size)));
    EXPECT_TRUE(std::ranges::all_of(ByteView{destination}.subspan(vector.size),
                                    [](std::byte value) { return value == Byte(0xaa); }));

    const auto decoded = DecodeSqliteVarint(ByteView{vector.encoded}.first(vector.size));
    ASSERT_TRUE(decoded.has_value()) << vector.value;
    EXPECT_EQ(vector.value, decoded->value);
    EXPECT_EQ(vector.size, decoded->bytes_consumed.value());
  }
}

TEST(CodingVarint, ChangesLengthAtEverySQLiteBoundary) {
  for (std::size_t short_length = 1; short_length < 8; ++short_length) {
    const auto boundary = std::uint64_t{1} << (short_length * 7U);
    EXPECT_EQ(short_length, SqliteVarintLength(boundary - 1U).value());
    EXPECT_EQ(short_length + 1U, SqliteVarintLength(boundary).value());
  }

  constexpr std::uint64_t kNineByteBoundary = std::uint64_t{1} << 56U;
  EXPECT_EQ(8U, SqliteVarintLength(kNineByteBoundary - 1U).value());
  EXPECT_EQ(9U, SqliteVarintLength(kNineByteBoundary).value());
}

TEST(CodingVarint, AcceptsSQLiteCompatibleOverlongEncodings) {
  constexpr std::array<std::byte, 2> kTwoByteZero{Byte(0x80), Byte(0x00)};
  constexpr std::array<std::byte, 3> kThreeByteOne{Byte(0x80), Byte(0x80), Byte(0x01)};
  constexpr std::array<std::byte, 9> kNineByte255{Byte(0x80), Byte(0x80), Byte(0x80),
                                                  Byte(0x80), Byte(0x80), Byte(0x80),
                                                  Byte(0x80), Byte(0x80), Byte(0xff)};

  const auto zero = DecodeSqliteVarint(kTwoByteZero);
  const auto one = DecodeSqliteVarint(kThreeByteOne);
  const auto two_hundred_fifty_five = DecodeSqliteVarint(kNineByte255);

  ASSERT_TRUE(zero.has_value());
  ASSERT_TRUE(one.has_value());
  ASSERT_TRUE(two_hundred_fifty_five.has_value());
  EXPECT_EQ(0U, zero->value);
  EXPECT_EQ(2U, zero->bytes_consumed.value());
  EXPECT_EQ(1U, one->value);
  EXPECT_EQ(3U, one->bytes_consumed.value());
  EXPECT_EQ(255U, two_hundred_fifty_five->value);
  EXPECT_EQ(9U, two_hundred_fifty_five->bytes_consumed.value());
}

TEST(CodingVarint, StopsAtTerminatorBeforeTrailingBytes) {
  constexpr std::array<std::byte, 3> kInput{Byte(0x81), Byte(0x00), Byte(0xff)};

  const auto decoded = DecodeSqliteVarint(kInput);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(128U, decoded->value);
  EXPECT_EQ(2U, decoded->bytes_consumed.value());
}

TEST(CodingVarint, RejectsTruncatedInput) {
  constexpr std::array<std::byte, 1> kOneByte{Byte(0x80)};
  constexpr std::array<std::byte, 2> kTwoBytes{Byte(0x81), Byte(0x80)};
  constexpr std::array<std::byte, 8> kEightBytes{Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80),
                                                 Byte(0x80), Byte(0x80), Byte(0x80), Byte(0x80)};

  for (const ByteView input :
       {ByteView{}, ByteView{kOneByte}, ByteView{kTwoBytes}, ByteView{kEightBytes}}) {
    const auto decoded = DecodeSqliteVarint(input);

    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(CodingError::kInputTooShort, decoded.error());
  }
}

TEST(CodingVarint, RejectsSmallDestinationWithoutMutation) {
  std::array<std::byte, 8> destination;
  destination.fill(Byte(0xaa));

  const auto encoded = EncodeSqliteVarint(std::numeric_limits<std::uint64_t>::max(), destination);

  ASSERT_FALSE(encoded.has_value());
  EXPECT_EQ(CodingError::kOutputTooSmall, encoded.error());
  EXPECT_TRUE(
      std::ranges::all_of(destination, [](std::byte value) { return value == Byte(0xaa); }));
}

void ExpectOverflow(const CodingResult<std::int64_t>& result) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(CodingError::kOverflow, result.error());
}

void ExpectValue(std::int64_t expected, const CodingResult<std::int64_t>& result) {
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(expected, *result);
}

TEST(CodingArithmetic, AddsWithoutUndefinedOverflow) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

  ExpectValue(kMax, CheckedAdd(kMax - 1, 1));
  ExpectValue(kMin, CheckedAdd(kMin + 1, -1));
  ExpectValue(0, CheckedAdd(-7, 7));
  ExpectOverflow(CheckedAdd(kMax, 1));
  ExpectOverflow(CheckedAdd(kMin, -1));
}

TEST(CodingArithmetic, SubtractsWithoutUndefinedOverflow) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

  ExpectValue(kMax, CheckedSubtract(-1, kMin));
  ExpectValue(0, CheckedSubtract(kMin, kMin));
  ExpectValue(-12, CheckedSubtract(-5, 7));
  ExpectOverflow(CheckedSubtract(kMax, -1));
  ExpectOverflow(CheckedSubtract(kMin, 1));
}

TEST(CodingArithmetic, MultipliesWithoutUndefinedOverflow) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

  ExpectValue(kMin, CheckedMultiply(kMin, 1));
  ExpectValue(9223372030926249001LL, CheckedMultiply(3037000499LL, 3037000499LL));
  ExpectValue(-42, CheckedMultiply(-6, 7));
  ExpectOverflow(CheckedMultiply(kMin, -1));
  ExpectOverflow(CheckedMultiply(kMax, 2));
  ExpectOverflow(CheckedMultiply(3037000500LL, 3037000500LL));
  ExpectOverflow(CheckedMultiply(-3037000500LL, -3037000500LL));
}

std::vector<std::byte> PatternBytes(std::size_t size) {
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index) {
    bytes[index] = Byte(static_cast<std::uint8_t>((index * 17U + 3U) & 0xffU));
  }
  return bytes;
}

TEST(CodingChecksum, MatchesRollbackJournalGoldenVectors) {
  constexpr std::uint32_t kSeed = 0x12345678U;
  const auto page_200 = PatternBytes(200);
  const auto page_201 = PatternBytes(201);
  const auto page_512 = PatternBytes(512);
  const auto page_4096 = PatternBytes(4096);

  EXPECT_EQ(0x12345678U, ComputeRollbackJournalChecksum(page_200, kSeed));
  EXPECT_EQ(0x1234568cU, ComputeRollbackJournalChecksum(page_201, kSeed));
  EXPECT_EQ(0x123457a6U, ComputeRollbackJournalChecksum(page_512, kSeed));
  EXPECT_EQ(0x123460a4U, ComputeRollbackJournalChecksum(page_4096, kSeed));
}

TEST(CodingChecksum, MatchesBigEndianWalGoldenVectorAndExtension) {
  std::vector<std::byte> sequential(24);
  for (std::size_t index = 0; index < sequential.size(); ++index) {
    sequential[index] = Byte(static_cast<std::uint8_t>(index));
  }

  const auto complete = ComputeWalChecksum(sequential, WalChecksumByteOrder::kBigEndian);
  const auto first =
      ComputeWalChecksum(ByteView{sequential}.first(8), WalChecksumByteOrder::kBigEndian);
  ASSERT_TRUE(first.has_value());
  const auto extended =
      ComputeWalChecksum(ByteView{sequential}.subspan(8), WalChecksumByteOrder::kBigEndian, *first);

  ASSERT_TRUE(complete.has_value());
  ASSERT_TRUE(extended.has_value());
  EXPECT_EQ((WalChecksum{944001116U, 1752993956U}), *complete);
  EXPECT_EQ(*complete, *extended);
}

TEST(CodingChecksum, MatchesLittleEndianWalGoldenVectorAndExtension) {
  std::vector<std::byte> sequential(24);
  for (std::size_t index = 0; index < sequential.size(); ++index) {
    sequential[index] = Byte(static_cast<std::uint8_t>(index));
  }

  const auto complete = ComputeWalChecksum(sequential, WalChecksumByteOrder::kLittleEndian);
  const auto first =
      ComputeWalChecksum(ByteView{sequential}.first(8), WalChecksumByteOrder::kLittleEndian);
  ASSERT_TRUE(first.has_value());
  const auto extended = ComputeWalChecksum(ByteView{sequential}.subspan(8),
                                           WalChecksumByteOrder::kLittleEndian, *first);

  ASSERT_TRUE(complete.has_value());
  ASSERT_TRUE(extended.has_value());
  EXPECT_EQ((WalChecksum{1548764216U, 2760932456U}), *complete);
  EXPECT_EQ(*complete, *extended);
}

TEST(CodingChecksum, RejectsInvalidWalChecksumLengths) {
  const std::array<std::byte, 9> bytes{};
  const std::vector<std::byte> oversized(65544);

  for (const ByteView input :
       {ByteView{}, ByteView{bytes}.first(7), ByteView{bytes}.first(9), ByteView{oversized}}) {
    const auto checksum = ComputeWalChecksum(input, WalChecksumByteOrder::kBigEndian);

    ASSERT_FALSE(checksum.has_value());
    EXPECT_EQ(CodingError::kInvalidChecksumLength, checksum.error());
  }
}

TEST(CodingChecksum, AcceptsMaximumWalChecksumLength) {
  const std::vector<std::byte> input(65536);

  const auto checksum = ComputeWalChecksum(input, WalChecksumByteOrder::kBigEndian);

  ASSERT_TRUE(checksum.has_value());
  EXPECT_EQ((WalChecksum{}), *checksum);
}

}  // namespace
}  // namespace modern_sqlite
