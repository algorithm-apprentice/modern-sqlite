#include "modern_sqlite/base/compiler_continuation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace modern_sqlite {
namespace {

using compiler_detail::ContinuationStack;

static_assert(!std::is_copy_constructible_v<ContinuationStack<int>>);
static_assert(!std::is_copy_assignable_v<ContinuationStack<int>>);
static_assert(!std::is_move_constructible_v<ContinuationStack<int>>);
static_assert(!std::is_move_assignable_v<ContinuationStack<int>>);

TEST(CompilerContinuation, StartsEmpty) {
  ContinuationStack<int> pending;
  EXPECT_TRUE(pending.empty());
  EXPECT_EQ(0U, pending.size());
}

TEST(CompilerContinuation, PreservesLifoAcrossInlineAndOverflowStorage) {
  ContinuationStack<int> pending;
  pending.Push(1);
  pending.Push(2);
  pending.Push(3);
  EXPECT_EQ(3U, pending.size());
  EXPECT_EQ(3, pending.Pop());
  EXPECT_EQ(2, pending.Pop());
  pending.Push(4);
  EXPECT_EQ(4, pending.Pop());
  EXPECT_EQ(1, pending.Pop());
  EXPECT_TRUE(pending.empty());
}

TEST(CompilerContinuation, TransfersMoveOnlyFramesWithoutCopying) {
  ContinuationStack<std::unique_ptr<int>> pending;
  for (int value = 0; value < 1000; ++value) {
    pending.Push(std::make_unique<int>(value));
  }
  for (int value = 999; value >= 0; --value) {
    std::unique_ptr<int> frame = pending.Pop();
    ASSERT_NE(nullptr, frame);
    EXPECT_EQ(value, *frame);
  }
  EXPECT_TRUE(pending.empty());
}

struct TrackedContinuation {
  int value;
  std::array<int, 4>& released;
  std::size_t& release_count;
  bool owns = true;

  TrackedContinuation(int id, std::array<int, 4>& events, std::size_t& count) noexcept
      : value(id), released(events), release_count(count) {}

  TrackedContinuation(const TrackedContinuation&) = delete;
  TrackedContinuation& operator=(const TrackedContinuation&) = delete;
  TrackedContinuation& operator=(TrackedContinuation&&) = delete;

  TrackedContinuation(TrackedContinuation&& other) noexcept
      : value(other.value),
        released(other.released),
        release_count(other.release_count),
        owns(std::exchange(other.owns, false)) {}

  ~TrackedContinuation() noexcept {
    if (owns) {
      released[release_count++] = value;
    }
  }
};

TEST(CompilerContinuation, ReleasesRetainedFramesInExplicitLifoOrder) {
  std::array<int, 4> released{};
  std::size_t count = 0;
  {
    ContinuationStack<TrackedContinuation> pending;
    pending.Push(TrackedContinuation{1, released, count});
    pending.Push(TrackedContinuation{2, released, count});
    pending.Push(TrackedContinuation{3, released, count});
  }
  EXPECT_EQ(3U, count);
  EXPECT_EQ((std::array<int, 4>{3, 2, 1, 0}), released);
}

TEST(CompilerContinuation, ReturnedFramesKeepOwnershipUntilTheirCallerReleasesThem) {
  std::array<int, 4> released{};
  std::size_t count = 0;
  {
    ContinuationStack<TrackedContinuation> pending;
    pending.Push(TrackedContinuation{1, released, count});
    pending.Push(TrackedContinuation{2, released, count});
    {
      TrackedContinuation frame = pending.Pop();
      EXPECT_EQ(2, frame.value);
      EXPECT_EQ(0U, count);
    }
    EXPECT_EQ(1U, count);
    EXPECT_EQ(1U, pending.size());
  }
  EXPECT_EQ(2U, count);
  EXPECT_EQ((std::array<int, 4>{2, 1, 0, 0}), released);
}

}  // namespace
}  // namespace modern_sqlite
