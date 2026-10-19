#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "src/temporary_storage/temporary_storage_internal.hpp"
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

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

class TopNEnvironment final {
 public:
  explicit TopNEnvironment(ByteCount threshold = ByteCount{1U << 20U})
      : pager_(TakeValue(Pager::Open(vfs_, test::kWritePagerInputPath))),
        factory_(TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                           TemporaryStorageOptions{
                                                               .mode = TemporaryStoreMode::kMemory,
                                                               .sorter_memory_threshold = threshold,
                                                           }))),
        storage_(std::in_place, TakeValue(factory_.CreateStatementStorage())) {
    RequireStatus(pager_->BeginRead());
  }

  [[nodiscard]] StatementTemporaryStorage& storage() noexcept {
    if (!storage_.has_value()) {
      std::terminate();
    }
    return *storage_;
  }
  [[nodiscard]] test::WritePagerFixedVfs& vfs() noexcept { return vfs_; }

 private:
  test::WritePagerFixedVfs vfs_;
  std::unique_ptr<Pager> pager_;
  TemporaryStorageFactory factory_;
  std::optional<StatementTemporaryStorage> storage_;
};

class FileTopNEnvironment final {
 public:
  explicit FileTopNEnvironment(ByteCount threshold = ByteCount{256})
      : pager_(TakeValue(
            Pager::Open(vfs_, test::kWritePagerInputPath,
                        PagerOptions{
                            .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                            .cache_capacity_pages = 64,
                        }))),
        factory_(TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                           TemporaryStorageOptions{
                                                               .mode = TemporaryStoreMode::kFile,
                                                               .sorter_memory_threshold = threshold,
                                                           }))),
        storage_(std::in_place, TakeValue(factory_.CreateStatementStorage())) {
    RequireStatus(pager_->BeginRead());
  }

  [[nodiscard]] StatementTemporaryStorage& storage() noexcept {
    if (!storage_.has_value()) {
      std::terminate();
    }
    return *storage_;
  }
  [[nodiscard]] test::WritePagerFixedVfs& vfs() noexcept { return vfs_; }
  void ResetStorage() noexcept { storage_.reset(); }

 private:
  test::WritePagerFixedVfs vfs_;
  std::unique_ptr<Pager> pager_;
  TemporaryStorageFactory factory_;
  std::optional<StatementTemporaryStorage> storage_;
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

[[nodiscard]] RecordSorterDescriptor DescendingNoCaseDescriptor() {
  return RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{NoCaseCollation(), IndexSortDirection::kDescending,
                               IndexNullPlacement::kLast},
          },
  };
}

[[nodiscard]] RecordSorterDescriptor WideDescriptor(std::size_t key_field_count) {
  return RecordSorterDescriptor{
      .field_count = key_field_count,
      .key_field_count = key_field_count,
      .key_columns =
          std::vector<IndexColumnOrder>(key_field_count, IndexColumnOrder{BinaryCollation()}),
  };
}

[[nodiscard]] EphemeralRelationDescriptor RelationDescriptor() {
  return EphemeralRelationDescriptor{
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

[[nodiscard]] ByteBuffer TextKey(std::size_t size, char fill) {
  const std::array<SqlValue, 1> values{SqlValue::Text(std::string(size, fill))};
  return Encode(values);
}

[[nodiscard]] ByteBuffer TextRow(std::size_t size, char fill, std::int64_t payload) {
  const std::array<SqlValue, 2> values{
      SqlValue::Text(std::string(size, fill)),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

struct BlobRowSpec {
  std::int64_t key;
  std::size_t payload_size;
  std::byte fill;
};

class FailingPressureTarget final : public TemporaryStorageSpillTarget {
 public:
  void Attach(TemporaryMemoryAccount& account) noexcept { account_ = &account; }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

  [[nodiscard]] ByteCount reclaimable_bytes() const noexcept override {
    return account_ == nullptr ? ByteCount{0} : account_->retained_bytes();
  }

  [[nodiscard]] Status SpillForPressure() override {
    ++calls_;
    return std::unexpected(
        Error::Create(ErrorCode::kTooLarge, "injected top-N pressure spill failure"));
  }

 private:
  TemporaryMemoryAccount* account_ = nullptr;
  std::size_t calls_ = 0;
};

[[nodiscard]] ByteBuffer BlobRow(BlobRowSpec spec) {
  ByteBuffer payload{ByteCount{spec.payload_size}};
  std::ranges::fill(payload.mutable_view(), spec.fill);
  std::array<SqlValue, 2> values{
      SqlValue::Integer(spec.key),
      SqlValue::Blob(std::move(payload)),
  };
  return Encode(values);
}

[[nodiscard]] std::int64_t IntegerField(const RecordView& record, std::size_t field) {
  return TakeValue(record.field(field)).integer_value().value_or(-1);
}

[[nodiscard]] ByteView BlobField(const RecordView& record, std::size_t field) {
  return TakeValue(record.field(field)).blob_value().value_or(ByteView{});
}

TEST(BoundedTopNApi, PublishesMoveOnlyLifecycleAndAdmissionResults) {
  EXPECT_EQ("writing", BoundedTopNStateName(BoundedTopNState::kWriting));
  EXPECT_EQ("candidate_pending", BoundedTopNStateName(BoundedTopNState::kCandidatePending));
  EXPECT_EQ("positioned", BoundedTopNStateName(BoundedTopNState::kPositioned));
  EXPECT_EQ("exhausted", BoundedTopNStateName(BoundedTopNState::kExhausted));
  EXPECT_EQ("closed", BoundedTopNStateName(BoundedTopNState::kClosed));
  EXPECT_EQ("unknown",
            BoundedTopNStateName(static_cast<BoundedTopNState>(255)));  // NOLINT

  TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 2));
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

TEST(BoundedTopNApi, ChecksSequenceExhaustionBeforeSignedConversion) {
  EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
            TakeValue(temporary_storage_detail::CheckedTopNSequence(
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));
  const auto exhausted = temporary_storage_detail::CheckedTopNSequence(
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U);
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, exhausted.error().code());
}

TEST(BoundedTopN, SequenceExhaustionPreservesTheFullQueueBeforeClosing) {
  TopNEnvironment environment{ByteCount{4U << 10U}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  BoundedTopN sibling = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(top_n.Insert(Row(2, 20)));

  Pager& relation_pager = temporary_storage_detail::BoundedTopNAccess::PagerFor(top_n);
  const PageNumber root_page = temporary_storage_detail::BoundedTopNAccess::RootPage(top_n);
  temporary_storage_detail::BoundedTopNAccess::SetNextSequence(
      top_n, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U);
  const auto exhausted = top_n.CheckCandidate(Key(1));
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, exhausted.error().code());
  EXPECT_FALSE(top_n.valid());
  EXPECT_FALSE(environment.storage().failed());

  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(relation_pager, root_page, columns));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const ByteBuffer wrapper = TakeValue(cursor.CopyPayload());
  const RecordView wrapper_view = TakeValue(RecordView::Parse(wrapper.view()));
  EXPECT_EQ(2, TakeValue(wrapper_view.field(0U)).integer_value());
  EXPECT_FALSE(TakeValue(cursor.Next()));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(sibling.CheckCandidate(Key(3))));
}

TEST(BoundedTopN, UsesMemoryBtreeWithoutOpeningAFileOrRetainingRows) {
  TopNEnvironment environment;
  EXPECT_EQ(0U, environment.storage().relation_cache_allocated_pages());

  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 2));
  EXPECT_EQ(0U, environment.vfs().pathless_open_count());
  EXPECT_GE(environment.storage().relation_cache_allocated_pages(), 2U);

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(top_n.Insert(Row(2, 20)));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(BoundedTopN, KeepsZeroBoundRootAndFileLazyInBothModes) {
  {
    TopNEnvironment environment;
    BoundedTopN zero = TakeValue(environment.storage().CreateTopN(Descriptor(), 0));
    EXPECT_EQ(0U, environment.storage().relation_cache_allocated_pages());
    EXPECT_EQ(0U, environment.vfs().pathless_open_count());
    EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(zero.CheckCandidate(Key(1))));
    RequireStatus(zero.Rewind());
    EXPECT_EQ(BoundedTopNState::kExhausted, zero.state());
  }
  {
    FileTopNEnvironment environment;
    BoundedTopN zero = TakeValue(environment.storage().CreateTopN(Descriptor(), 0));
    EXPECT_EQ(0U, environment.vfs().pathless_open_count());
    EXPECT_EQ(0U, environment.storage().live_file_count());
    EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(zero.CheckCandidate(Key(1))));
  }
}

TEST(BoundedTopN, ReservesQueueSuffixFieldsForPositiveBounds) {
  {
    TopNEnvironment accepted;
    BoundedTopN top_n =
        TakeValue(accepted.storage().CreateTopN(WideDescriptor(kMaximumRecordFieldCount - 2U), 1));
    EXPECT_TRUE(top_n.valid());
  }
  {
    TopNEnvironment rejected;
    const auto top_n =
        rejected.storage().CreateTopN(WideDescriptor(kMaximumRecordFieldCount - 1U), 1);
    ASSERT_FALSE(top_n.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, top_n.error().code());
    EXPECT_EQ(0U, rejected.storage().relation_cache_allocated_pages());
  }
  {
    TopNEnvironment zero;
    BoundedTopN top_n =
        TakeValue(zero.storage().CreateTopN(WideDescriptor(kMaximumRecordFieldCount - 1U), 0));
    EXPECT_TRUE(top_n.valid());
    EXPECT_EQ(0U, zero.storage().relation_cache_allocated_pages());
  }
}

TEST(BoundedTopN, AdmitsCompetitiveKeysAndEvictsLargestBeforePayloadInsertion) {
  TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 2));

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
  TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 2));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_TRUE(top_n.Insert(Row(1, 10)).has_value());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_TRUE(top_n.Insert(Row(1, 11)).has_value());
  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(top_n.CheckCandidate(Key(1))));
  ASSERT_TRUE(top_n.Rewind().has_value());
  EXPECT_EQ(10, IntegerField(TakeValue(top_n.current_record()), 1));
  EXPECT_TRUE(TakeValue(top_n.Next()));
  EXPECT_EQ(11, IntegerField(TakeValue(top_n.current_record()), 1));

  BoundedTopN zero = TakeValue(environment.storage().CreateTopN(Descriptor(), 0));
  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(zero.CheckCandidate(Key(1))));
  EXPECT_TRUE(zero.Rewind().has_value());
  EXPECT_EQ(BoundedTopNState::kExhausted, zero.state());
}

TEST(BoundedTopN, EnforcesPendingCandidateAndLifecycleRules) {
  TopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
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

TEST(BoundedTopN, MemoryBtreeRequiresDatabaseAndRootPages) {
  TopNEnvironment insufficient{ByteCount{test::kWritePagerPageSize}};
  const auto rejected = insufficient.storage().CreateTopN(Descriptor(), 1);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  EXPECT_TRUE(insufficient.storage().failed());
  EXPECT_EQ(0U, insufficient.vfs().pathless_open_count());

  TopNEnvironment exact{ByteCount{2U * test::kWritePagerPageSize}};
  auto created = exact.storage().CreateTopN(Descriptor(), 1);
  ASSERT_TRUE(created.has_value()) << created.error().ToString();
  BoundedTopN top_n = std::move(*created);
  EXPECT_EQ(2U, exact.storage().relation_cache_allocated_pages());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  const Status inserted = top_n.Insert(Row(1, 10));
  ASSERT_TRUE(inserted.has_value()) << inserted.error().ToString();
}

TEST(BoundedTopN, AccountsPendingCandidatesButNotRetainedBtreeRows) {
  TopNEnvironment environment{ByteCount{2U << 10U}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));

  ByteBuffer first_key = Key(2);
  const ByteCount first_key_bytes = first_key.capacity();
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(std::move(first_key))));
  EXPECT_EQ(first_key_bytes, top_n.memory_usage());
  EXPECT_EQ(top_n.memory_usage(), environment.storage().retained_bytes());

  RequireStatus(top_n.Insert(Row(2, 20)));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());

  ByteBuffer replacement_key = Key(1);
  const ByteCount replacement_key_bytes = replacement_key.capacity();
  EXPECT_EQ(TopNCheckResult::kAccepted,
            TakeValue(top_n.CheckCandidate(std::move(replacement_key))));
  EXPECT_EQ(replacement_key_bytes, top_n.memory_usage());
  EXPECT_EQ(top_n.memory_usage(), environment.storage().retained_bytes());
  RequireStatus(top_n.Insert(Row(1, 10)));
  EXPECT_EQ(1U, top_n.record_count());
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());

  RequireStatus(top_n.Rewind());
  EXPECT_EQ(1, IntegerField(TakeValue(top_n.current_record()), 0));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());

  RequireStatus(top_n.Reset());
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(BoundedTopN, EnforcesOneStatementBudgetAcrossPendingMemoryKeys) {
  TopNEnvironment environment{ByteCount{4U << 10U}};
  BoundedTopN first = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  BoundedTopN second = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(first.CheckCandidate(TextKey(3'000U, 'a'))));
  const ByteCount first_bytes = first.memory_usage();
  EXPECT_GE(first_bytes, ByteCount{3'000U});

  const auto rejected = second.CheckCandidate(TextKey(3'000U, 'b'));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  EXPECT_TRUE(second.valid());
  EXPECT_TRUE(first.valid());
  EXPECT_EQ(first_bytes, environment.storage().retained_bytes());
}

TEST(BoundedTopN, FullCandidateBudgetFailurePreservesTheLargestRecord) {
  TopNEnvironment environment{ByteCount{4U << 10U}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(TextKey(16U, 'z'))));
  RequireStatus(top_n.Insert(TextRow(16U, 'z', 10)));

  const auto rejected = top_n.CheckCandidate(TextKey(5'000U, 'a'));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  EXPECT_TRUE(top_n.valid());
  EXPECT_EQ(1U, top_n.record_count());
  EXPECT_FALSE(top_n.has_pending_candidate());

  RequireStatus(top_n.Rewind());
  const RecordFieldView key = TakeValue(TakeValue(top_n.current_record()).field(0U));
  ASSERT_TRUE(key.text_value().has_value());
  EXPECT_EQ(std::string(16U, 'z'), key.text_value()->bytes());
}

TEST(BoundedTopN, AbandonedEvictionPoisonsSiblingsButResetRecoversTheRoot) {
  {
    TopNEnvironment environment{ByteCount{4U << 10U}};
    BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
    BoundedTopN sibling = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
    EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
    RequireStatus(top_n.Insert(Row(2, 20)));
    EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
    top_n.Close();
    EXPECT_TRUE(environment.storage().failed());
    const auto rejected = sibling.CheckCandidate(Key(3));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
    EXPECT_FALSE(sibling.valid());
  }

  TopNEnvironment environment{ByteCount{4U << 10U}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  BoundedTopN sibling = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(top_n.Insert(Row(2, 20)));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  RequireStatus(top_n.Reset());
  EXPECT_FALSE(environment.storage().failed());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(sibling.CheckCandidate(Key(3))));
}

TEST(BoundedTopN, AccountsPendingAndOversizedFileDrainWorkingSets) {
  FileTopNEnvironment environment{ByteCount{128}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());

  ByteBuffer key = Key(1);
  const ByteCount key_bytes = key.capacity();
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(std::move(key))));
  EXPECT_EQ(key_bytes, top_n.memory_usage());
  EXPECT_EQ(top_n.memory_usage(), environment.storage().retained_bytes());

  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U, .fill = std::byte{0x11}})));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());

  RequireStatus(top_n.Rewind());
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  const RecordView current = TakeValue(top_n.current_record());
  EXPECT_EQ(1, IntegerField(current, 0));
  EXPECT_GT(top_n.memory_usage(), environment.storage().memory_budget());
  EXPECT_EQ(top_n.memory_usage(), environment.storage().retained_bytes());
  EXPECT_FALSE(TakeValue(top_n.Next()));
  EXPECT_EQ(ByteCount{0}, top_n.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(BoundedTopN, PreservesPressureSpillFailureBeforeWorkingSetFallback) {
  FileTopNEnvironment environment{ByteCount{128}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U, .fill = std::byte{0x11}})));

  FailingPressureTarget target;
  TemporaryMemoryAccount occupied = TakeValue(environment.storage().RegisterMemoryOwner(target));
  target.Attach(occupied);
  RequireStatus(occupied.Reserve(environment.storage().memory_budget()));

  RequireStatus(top_n.Rewind());
  const auto current = top_n.current_record();
  ASSERT_FALSE(current.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, current.error().code()) << current.error().ToString();
  EXPECT_EQ(1U, target.calls());
  EXPECT_TRUE(environment.storage().failed());
  EXPECT_FALSE(top_n.valid());
}

TEST(BoundedTopNApi, KeepsBorrowedStatementStateStableAcrossStorageMove) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{256},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  BoundedTopN top_n = TakeValue(storage.CreateTopN(Descriptor(), 1));
  const StatementTemporaryStorage moved{std::move(storage)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(storage.valid());

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  RequireStatus(top_n.Insert(Row(1, 10)));
  EXPECT_EQ(1U, moved.live_file_count());
  top_n.Close();
}

TEST(BoundedTopN, SharesOneEagerStatementArenaAcrossFileModeTopNs) {
  FileTopNEnvironment environment;
  BoundedTopN first = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  BoundedTopN second = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));

  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(first.CheckCandidate(Key(2))));
  RequireStatus(first.Insert(Row(2, 20)));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(second.CheckCandidate(Key(1))));
  RequireStatus(second.Insert(Row(1, 10)));
  RequireStatus(first.Rewind());
  RequireStatus(second.Rewind());
  EXPECT_EQ(2, IntegerField(TakeValue(first.current_record()), 0));
  EXPECT_EQ(1, IntegerField(TakeValue(second.current_record()), 0));

  first.Close();
  second.Close();
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(1U, environment.storage().live_file_count());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(BoundedTopN, SharesAndReusesTheRelationArenaAcrossCapabilityTypes) {
  FileTopNEnvironment environment{ByteCount{256}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(RelationDescriptor()));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(top_n.Insert(Row(2, 20)));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(relation.Insert(
                BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U, .fill = std::byte{0x11}}),
                EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(relation.file_backed());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());

  RequireStatus(top_n.Rewind());
  RequireStatus(relation.Rewind());
  EXPECT_EQ(2, IntegerField(TakeValue(top_n.current_record()), 0));
  EXPECT_EQ(1, IntegerField(TakeValue(relation.current_record()), 0));

  top_n.Close();
  relation.Close();
  const ByteCount overhead = environment.storage().overhead_bytes();
  BoundedTopN reused = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(reused.CheckCandidate(Key(3))));
  RequireStatus(reused.Insert(Row(3, 30)));
  RequireStatus(reused.Rewind());
  EXPECT_EQ(3, IntegerField(TakeValue(reused.current_record()), 0));
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  reused.Close();
  EXPECT_EQ(overhead, environment.storage().overhead_bytes());
}

TEST(BoundedTopN, ReusesMemoryRootsWithFreshOrderingMetadata) {
  TopNEnvironment environment{ByteCount{4U << 10U}};
  {
    BoundedTopN first = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
    EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(first.CheckCandidate(Key(1))));
    RequireStatus(first.Insert(Row(1, 10)));
  }
  const std::size_t allocated_pages = environment.storage().relation_cache_allocated_pages();

  BoundedTopN reused = TakeValue(environment.storage().CreateTopN(DescendingNoCaseDescriptor(), 2));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(reused.CheckCandidate(TextKey(5U, 'a'))));
  RequireStatus(reused.Insert(TextRow(5U, 'a', 10)));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(reused.CheckCandidate(TextKey(4U, 'B'))));
  RequireStatus(reused.Insert(TextRow(4U, 'B', 20)));
  RequireStatus(reused.Rewind());
  const RecordFieldView first = TakeValue(TakeValue(reused.current_record()).field(0U));
  ASSERT_TRUE(first.text_value().has_value());
  EXPECT_EQ("BBBB", first.text_value()->bytes());
  EXPECT_TRUE(TakeValue(reused.Next()));
  const RecordFieldView second = TakeValue(TakeValue(reused.current_record()).field(0U));
  ASSERT_TRUE(second.text_value().has_value());
  EXPECT_EQ("aaaaa", second.text_value()->bytes());
  EXPECT_EQ(allocated_pages, environment.storage().relation_cache_allocated_pages());
}

TEST(BoundedTopN, FileModeSpillsStableRowsAndClearsTheEphemeralTreeOnReset) {
  FileTopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 3));
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  const std::optional<FileOpenOptions> open_options =
      environment.vfs().last_pathless_open_options();
  ASSERT_TRUE(open_options.has_value());
  EXPECT_EQ((FileOpenOptions{
                .kind = FileKind::kTransientDatabase,
                .access = FileAccessMode::kReadWrite,
                .create = true,
                .exclusive_create = true,
                .delete_on_close = true,
            }),
            open_options.value_or(FileOpenOptions{}));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 700U, .fill = std::byte{0x20}})));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 700U, .fill = std::byte{0x11}})));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 700U, .fill = std::byte{0x12}})));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_EQ(2U, top_n.record_count());
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 700U, .fill = std::byte{0x13}})));
  EXPECT_EQ(TopNCheckResult::kRejected, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_GT(environment.vfs().total_subjournal_writes(), 0U);

  RequireStatus(top_n.Rewind());
  for (const std::byte expected : {std::byte{0x11}, std::byte{0x12}, std::byte{0x13}}) {
    const RecordView current = TakeValue(top_n.current_record());
    EXPECT_EQ(1, IntegerField(current, 0));
    const ByteView payload = BlobField(current, 1);
    ASSERT_EQ(700U, payload.size());
    EXPECT_EQ(expected, payload.front());
    const bool advanced = TakeValue(top_n.Next());
    EXPECT_EQ(expected != std::byte{0x13}, advanced);
  }

  RequireStatus(top_n.Reset());
  EXPECT_EQ(BoundedTopNState::kWriting, top_n.state());
  EXPECT_EQ(0U, top_n.record_count());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(4))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 4, .payload_size = 900U, .fill = std::byte{0x44}})));
  RequireStatus(top_n.Rewind());
  EXPECT_EQ(4, IntegerField(TakeValue(top_n.current_record()), 0));
  RequireStatus(top_n.Reset());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(5))));
  RequireStatus(top_n.Reset());
  EXPECT_FALSE(top_n.has_pending_candidate());
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(6))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 6, .payload_size = 16U, .fill = std::byte{0x66}})));
  RequireStatus(top_n.Rewind());
  EXPECT_EQ(6, IntegerField(TakeValue(top_n.current_record()), 0));

  top_n.Close();
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(BoundedTopN, FileModePropagatesFailuresAndDefersSharedFileCleanup) {
  FileTopNEnvironment open_failure;
  open_failure.vfs().FailNextPathlessOpen(ErrorCode::kIo);
  const auto failed_open = open_failure.storage().CreateTopN(Descriptor(), 2);
  ASSERT_FALSE(failed_open.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_open.error().code());
  EXPECT_TRUE(open_failure.storage().failed());
  EXPECT_FALSE(open_failure.vfs().pathless_file_present());

  FileTopNEnvironment write_failure;
  BoundedTopN writing = TakeValue(write_failure.storage().CreateTopN(Descriptor(), 2));
  BoundedTopN sibling = TakeValue(write_failure.storage().CreateTopN(Descriptor(), 2));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(writing.CheckCandidate(Key(1))));
  write_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const Status failed_insert =
      writing.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 700U, .fill = std::byte{0x11}}));
  ASSERT_FALSE(failed_insert.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_insert.error().code());
  EXPECT_FALSE(writing.valid());
  EXPECT_TRUE(write_failure.storage().failed());
  const auto sibling_after_failure = sibling.CheckCandidate(Key(2));
  ASSERT_FALSE(sibling_after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, sibling_after_failure.error().code());
  EXPECT_FALSE(sibling.valid());
  EXPECT_TRUE(write_failure.vfs().pathless_file_present());
  write_failure.ResetStorage();
  EXPECT_FALSE(write_failure.vfs().pathless_file_present());

  FileTopNEnvironment read_failure;
  BoundedTopN reading = TakeValue(read_failure.storage().CreateTopN(Descriptor(), 4));
  for (std::int64_t key = 4; key >= 1; --key) {
    EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(reading.CheckCandidate(Key(key))));
    RequireStatus(reading.Insert(
        BlobRow(BlobRowSpec{.key = key,
                            .payload_size = 900U,
                            .fill = static_cast<std::byte>(static_cast<std::uint8_t>(key))})));
  }
  read_failure.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);
  RequireStatus(reading.Rewind());
  const auto failed_current = reading.current_record();
  ASSERT_FALSE(failed_current.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_current.error().code());
  EXPECT_FALSE(reading.valid());
  EXPECT_TRUE(read_failure.storage().failed());
  EXPECT_TRUE(read_failure.vfs().pathless_file_present());
  read_failure.ResetStorage();
  EXPECT_FALSE(read_failure.vfs().pathless_file_present());
}

TEST(BoundedTopN, PostEvictionInsertFailurePoisonsTheSharedArena) {
  FileTopNEnvironment environment{ByteCount{128}};
  BoundedTopN top_n = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  BoundedTopN sibling = TakeValue(environment.storage().CreateTopN(Descriptor(), 1));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(2))));
  RequireStatus(
      top_n.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 900U, .fill = std::byte{0x22}})));

  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(top_n.CheckCandidate(Key(1))));
  EXPECT_EQ(0U, top_n.record_count());
  environment.vfs().FailPathlessWriteAfter(0U, ErrorCode::kIo);
  const Status failed =
      top_n.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U, .fill = std::byte{0x11}}));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(top_n.valid());
  EXPECT_TRUE(environment.storage().failed());

  const auto sibling_result = sibling.CheckCandidate(Key(3));
  ASSERT_FALSE(sibling_result.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, sibling_result.error().code());
  EXPECT_FALSE(sibling.valid());
}

}  // namespace
}  // namespace modern_sqlite
