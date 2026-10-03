#include "modern_sqlite/base/bytes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace modern_sqlite {
namespace {

static_assert(!std::same_as<ByteOffset, ByteCount>);
static_assert(std::same_as<ByteView, std::span<const std::byte>>);
static_assert(std::same_as<MutableByteView, std::span<std::byte>>);
static_assert(sizeof(ByteOffset) == sizeof(std::size_t));
static_assert(sizeof(ByteCount) == sizeof(std::size_t));
static_assert(!std::copy_constructible<ByteBuffer>);
static_assert(std::movable<ByteBuffer>);

TEST(Bytes, CreatesEmptyTextView) {
  const ByteView bytes = AsBytes(std::string_view{});

  EXPECT_TRUE(bytes.empty());
  EXPECT_TRUE(AsStringView(bytes).empty());
}

TEST(Bytes, PreservesEmbeddedNullBytesWithoutCopying) {
  const std::string text{"a\0b", 3};
  const ByteView bytes = AsBytes(text);

  ASSERT_EQ(3U, bytes.size());
  EXPECT_EQ(std::byte{'a'}, bytes[0]);
  EXPECT_EQ(std::byte{0}, bytes[1]);
  EXPECT_EQ(std::byte{'b'}, bytes[2]);
  EXPECT_EQ(text.data(), AsStringView(bytes).data());
}

TEST(Bytes, MutableTextViewUpdatesOriginalStorage) {
  std::array<char, 3> text{'a', 'b', 'c'};
  const MutableByteView bytes = AsWritableBytes(text);

  bytes[1] = std::byte{'x'};

  EXPECT_EQ((std::array<char, 3>{'a', 'x', 'c'}), text);
}

TEST(Bytes, CheckedSliceAcceptsBoundaries) {
  const std::string text = "abcd";
  const ByteView bytes = AsBytes(text);

  const auto full = Slice(bytes, ByteOffset{0}, ByteCount{4});
  const auto empty_suffix = Slice(bytes, ByteOffset{4}, ByteCount{0});

  ASSERT_TRUE(full.has_value());
  EXPECT_EQ("abcd", AsStringView(*full));
  ASSERT_TRUE(empty_suffix.has_value());
  EXPECT_TRUE(empty_suffix->empty());
}

TEST(Bytes, CheckedSliceRejectsInvalidAndOverflowingRanges) {
  const ByteView bytes = AsBytes("abcd");

  const auto past_end = Slice(bytes, ByteOffset{5}, ByteCount{0});
  const auto too_long = Slice(bytes, ByteOffset{3}, ByteCount{2});
  const auto overflowing =
      Slice(bytes, ByteOffset{std::numeric_limits<std::size_t>::max()}, ByteCount{2});

  ASSERT_FALSE(past_end.has_value());
  EXPECT_EQ(ByteError::kOutOfRange, past_end.error());
  ASSERT_FALSE(too_long.has_value());
  EXPECT_EQ(ByteError::kOutOfRange, too_long.error());
  ASSERT_FALSE(overflowing.has_value());
  EXPECT_EQ(ByteError::kOutOfRange, overflowing.error());
}

TEST(Bytes, MutableSliceUpdatesOriginalStorage) {
  std::array<std::byte, 3> storage{
      std::byte{'a'},
      std::byte{'b'},
      std::byte{'c'},
  };

  auto middle = Slice(MutableByteView{storage}, ByteOffset{1}, ByteCount{1});

  ASSERT_TRUE(middle.has_value());
  (*middle)[0] = std::byte{'x'};
  EXPECT_EQ(std::byte{'x'}, storage[1]);
}

TEST(ByteBuffer, AllocatesZeroInitializedStorage) {
  const ByteBuffer buffer{ByteCount{3}};

  EXPECT_EQ(3U, buffer.size().value());
  EXPECT_FALSE(buffer.empty());
  EXPECT_EQ((std::array<std::byte, 3>{std::byte{0}, std::byte{0}, std::byte{0}}),
            (std::array<std::byte, 3>{
                buffer.view()[0],
                buffer.view()[1],
                buffer.view()[2],
            }));
}

TEST(ByteBuffer, DefaultConstructionIsEmpty) {
  ByteBuffer buffer;

  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(0U, buffer.size().value());
  EXPECT_TRUE(buffer.view().empty());
  EXPECT_TRUE(buffer.mutable_view().empty());
}

TEST(ByteBuffer, CopyAndCloneAreExplicitAndIndependent) {
  ByteBuffer buffer = ByteBuffer::CopyOf(AsBytes("abc"));
  const ByteBuffer clone = buffer.Clone();

  buffer.mutable_view()[1] = std::byte{'x'};

  EXPECT_EQ("axc", AsStringView(buffer.view()));
  EXPECT_EQ("abc", AsStringView(clone.view()));
}

TEST(ByteBuffer, MovesOwnedStorage) {
  ByteBuffer original = ByteBuffer::CopyOf(AsBytes("abc"));
  const std::byte* original_data = original.view().data();

  const ByteBuffer moved = std::move(original);

  EXPECT_EQ(original_data, moved.view().data());
  EXPECT_EQ("abc", AsStringView(moved.view()));
}

TEST(CopyBytes, SupportsForwardOverlap) {
  ByteBuffer buffer = ByteBuffer::CopyOf(AsBytes("abcdef"));
  const ByteView source = buffer.view().first(4);
  const MutableByteView destination = buffer.mutable_view().subspan(2, 4);

  const auto result = CopyBytes(destination, source);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("ababcd", AsStringView(buffer.view()));
}

TEST(CopyBytes, SupportsBackwardOverlap) {
  ByteBuffer buffer = ByteBuffer::CopyOf(AsBytes("abcdef"));
  const ByteView source = buffer.view().subspan(2, 4);
  const MutableByteView destination = buffer.mutable_view().first(4);

  const auto result = CopyBytes(destination, source);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("cdefef", AsStringView(buffer.view()));
}

TEST(CopyBytes, RejectsSmallDestinationWithoutMutation) {
  std::array<std::byte, 3> destination{
      std::byte{'x'},
      std::byte{'y'},
      std::byte{'z'},
  };
  const auto original = destination;

  const auto result = CopyBytes(destination, AsBytes("abcd"));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ByteError::kDestinationTooSmall, result.error());
  EXPECT_EQ(original, destination);
}

TEST(CopyBytes, AcceptsEmptyViews) {
  const auto result = CopyBytes(MutableByteView{}, ByteView{});

  EXPECT_TRUE(result.has_value());
}

}  // namespace
}  // namespace modern_sqlite
