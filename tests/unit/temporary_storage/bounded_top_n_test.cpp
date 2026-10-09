#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
                                                           }))) {}

  [[nodiscard]] const TemporaryStorageFactory& factory() const noexcept { return factory_; }

 private:
  test::WritePagerFixedVfs vfs_;
  std::unique_ptr<Pager> pager_;
  TemporaryStorageFactory factory_;
};

class FileTopNEnvironment final {
 public:
  explicit FileTopNEnvironment(ByteCount threshold = ByteCount{1})
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
                                                           }))) {
    RequireStatus(pager_->BeginRead());
  }

  [[nodiscard]] const TemporaryStorageFactory& factory() const noexcept { return factory_; }
  [[nodiscard]] test::WritePagerFixedVfs& vfs() noexcept { return vfs_; }

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

struct BlobRowSpec {
  std::int64_t key;
  std::size_t payload_size;
  std::byte fill;
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

TEST(BoundedTopN, FileModeSpillsStableRowsAndClearsTheEphemeralTreeOnReset) {
  FileTopNEnvironment environment;
  BoundedTopN top_n = TakeValue(environment.factory().CreateTopN(Descriptor(), 3));
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
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(BoundedTopN, FileModePropagatesOpenWriteAndReadFailuresAndDeletesTheFile) {
  FileTopNEnvironment open_failure;
  open_failure.vfs().FailNextPathlessOpen(ErrorCode::kIo);
  const auto failed_open = open_failure.factory().CreateTopN(Descriptor(), 2);
  ASSERT_FALSE(failed_open.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_open.error().code());
  EXPECT_FALSE(open_failure.vfs().pathless_file_present());

  FileTopNEnvironment write_failure;
  BoundedTopN writing = TakeValue(write_failure.factory().CreateTopN(Descriptor(), 2));
  EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(writing.CheckCandidate(Key(1))));
  write_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const Status failed_insert =
      writing.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 700U, .fill = std::byte{0x11}}));
  ASSERT_FALSE(failed_insert.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_insert.error().code());
  EXPECT_FALSE(writing.valid());
  EXPECT_FALSE(write_failure.vfs().pathless_file_present());

  FileTopNEnvironment read_failure;
  BoundedTopN reading = TakeValue(read_failure.factory().CreateTopN(Descriptor(), 4));
  for (std::int64_t key = 4; key >= 1; --key) {
    EXPECT_EQ(TopNCheckResult::kAccepted, TakeValue(reading.CheckCandidate(Key(key))));
    RequireStatus(reading.Insert(
        BlobRow(BlobRowSpec{.key = key,
                            .payload_size = 900U,
                            .fill = static_cast<std::byte>(static_cast<std::uint8_t>(key))})));
  }
  read_failure.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);
  const Status failed_rewind = reading.Rewind();
  ASSERT_FALSE(failed_rewind.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_rewind.error().code());
  EXPECT_FALSE(reading.valid());
  EXPECT_FALSE(read_failure.vfs().pathless_file_present());
}

}  // namespace
}  // namespace modern_sqlite
