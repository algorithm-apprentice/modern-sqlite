#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<BoundedTopN>);
static_assert(std::is_nothrow_move_constructible_v<BoundedTopN>);

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

class TopNEnvironment final {
 public:
  explicit TopNEnvironment(ByteCount threshold = ByteCount{1U << 20U})
      : pager_(TakeValue(Pager::Open(vfs_, test::kWritePagerInputPath))),
        factory_(TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                           TemporaryStorageOptions{
                                                               .mode = TemporaryStoreMode::kMemory,
                                                               .sorter_memory_threshold = threshold,
                                                           }))) {}

  [[nodiscard]] const TemporaryStorageFactory& factory() const noexcept { return factory_; }

 private:
  test::WritePagerFixedVfs vfs_;
  std::unique_ptr<Pager> pager_;
  TemporaryStorageFactory factory_;
};

[[nodiscard]] RecordSorterDescriptor Descriptor() {
  return RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  };
}

[[nodiscard]] ByteBuffer Encode(std::span<const SqlValue> values) {
  return TakeValue(EncodeRecord(values));
}

[[nodiscard]] ByteBuffer Key(std::int64_t key) {
  const std::array<SqlValue, 1> values{SqlValue::Integer(key)};
  return Encode(values);
}

[[nodiscard]] ByteBuffer Row(std::int64_t key, std::int64_t payload) {
  const std::array<SqlValue, 2> values{
      SqlValue::Integer(key),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

[[nodiscard]] std::int64_t IntegerField(const RecordView& record, std::size_t field) {
  return TakeValue(record.field(field)).integer_value().value_or(-1);
}

TEST(BoundedTopNApi, PublishesMoveOnlyLifecycleAndAdmissionResults) {
  EXPECT_EQ("writing", BoundedTopNStateName(BoundedTopNState::kWriting));
  EXPECT_EQ("candidate_pending", BoundedTopNStateName(BoundedTopNState::kCandidatePending));
  EXPECT_EQ("positioned", BoundedTopNStateName(BoundedTopNState::kPositioned));
  EXPECT_EQ("exhausted", BoundedTopNStateName(BoundedTopNState::kExhausted));
  EXPECT_EQ("closed", BoundedTopNStateName(BoundedTopNState::kClosed));
  EXPECT_EQ("unknown",
            BoundedTopNStateName(static_cast<BoundedTopNState>(255)));  // NOLINT

  const TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 2));
  EXPECT_TRUE(top_n.valid());
  EXPECT_EQ(BoundedTopNState::kWriting, top_n.state());
  EXPECT_EQ(2U, top_n.bound());
  EXPECT_EQ(0U, top_n.record_count());
  EXPECT_FALSE(top_n.has_pending_candidate());

  BoundedTopN moved = std::move(top_n);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(top_n.valid());
  EXPECT_EQ(BoundedTopNState::kClosed, top_n.state());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  moved.Close();
  EXPECT_FALSE(moved.valid());
}

TEST(BoundedTopN, AdmitsCompetitiveKeysAndEvictsLargestBeforePayloadInsertion) {
  const TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 2));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(3))));
  EXPECT_TRUE(top_n.has_pending_candidate());
  EXPECT_TRUE(top_n.Insert(Row(3, 30)).has_value());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_TRUE(top_n.Insert(Row(1, 10)).has_value());
  EXPECT_EQ(2U, top_n.record_count());

  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(top_n.CheckCandidate(Key(4))));
  EXPECT_FALSE(top_n.has_pending_candidate());
  EXPECT_EQ(2U, top_n.record_count());

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  EXPECT_TRUE(top_n.has_pending_candidate());
  EXPECT_EQ(1U, top_n.record_count());
  EXPECT_TRUE(top_n.Insert(Row(2, 20)).has_value());
  EXPECT_EQ(2U, top_n.record_count());

  ASSERT_TRUE(top_n.Rewind().has_value());
  EXPECT_EQ(1, IntegerField(TakeValue(top_n.current_record()), 0));
  EXPECT_TRUE(TakeValue(top_n.Next()));
  EXPECT_EQ(2, IntegerField(TakeValue(top_n.current_record()), 0));
  EXPECT_FALSE(TakeValue(top_n.Next()));
}

TEST(BoundedTopN, RetainsEarlierRowsForEqualKeysAndSupportsZeroBound) {
  const TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 2));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_TRUE(top_n.Insert(Row(1, 10)).has_value());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_TRUE(top_n.Insert(Row(1, 11)).has_value());
  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(top_n.CheckCandidate(Key(1))));
  ASSERT_TRUE(top_n.Rewind().has_value());
  EXPECT_EQ(10, IntegerField(TakeValue(top_n.current_record()), 1));
  EXPECT_TRUE(TakeValue(top_n.Next()));
  EXPECT_EQ(11, IntegerField(TakeValue(top_n.current_record()), 1));

  BoundedTopN zero = TakeValue(environment.factory().CreateTopN(Descriptor(), 0));
  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(zero.CheckCandidate(Key(1))));
  EXPECT_TRUE(zero.Rewind().has_value());
  EXPECT_EQ(BoundedTopNState::kExhausted, zero.state());
}

TEST(BoundedTopN, EnforcesPendingCandidateAndLifecycleRules) {
  const TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 1));
  EXPECT_FALSE(top_n.Insert(Row(1, 10)).has_value());
  EXPECT_FALSE(top_n.current_record().has_value());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_FALSE(top_n.CheckCandidate(Key(2)).has_value());
  EXPECT_FALSE(top_n.Rewind().has_value());
  EXPECT_FALSE(top_n.Insert(Row(2, 20)).has_value());
  EXPECT_TRUE(top_n.Insert(Row(1, 10)).has_value());
  EXPECT_TRUE(top_n.Rewind().has_value());
  EXPECT_FALSE(top_n.CheckCandidate(Key(0)).has_value());
  EXPECT_TRUE(top_n.Reset().has_value());
  EXPECT_EQ(0U, top_n.record_count());
  EXPECT_EQ(BoundedTopNState::kWriting, top_n.state());
  top_n.Close();
  EXPECT_FALSE(top_n.Reset().has_value());
}

TEST(BoundedTopN, EnforcesMemoryThresholdWithoutPublishingPartialRows) {
  const TopNEnvironment environment{ByteCount{1}};
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 1));
  const auto checked = top_n.CheckCandidate(Key(1));
  ASSERT_FALSE(checked.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, checked.error().code());
  EXPECT_TRUE(top_n.valid());
  EXPECT_EQ(0U, top_n.record_count());
  EXPECT_FALSE(top_n.has_pending_candidate());
}

}  // namespace
}  // namespace modern_sqlite
