#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<RecordSorter>);
static_assert(!std::is_copy_assignable_v<RecordSorter>);
static_assert(std::is_nothrow_move_constructible_v<RecordSorter>);
static_assert(std::is_nothrow_move_assignable_v<RecordSorter>);

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

class SorterEnvironment final {
 public:
  explicit SorterEnvironment(ByteCount threshold = ByteCount{1U << 20U},
                             TemporaryStoreMode mode = TemporaryStoreMode::kMemory)
      : pager_(TakeValue(Pager::Open(vfs_, test::kWritePagerInputPath))),
        factory_(TakeValue(TemporaryStorageFactory::Create(vfs_, BeginRead(*pager_),
                                                           TemporaryStorageOptions{
                                                               .mode = mode,
                                                               .sorter_memory_threshold = threshold,
                                                           }))),
        storage_(std::in_place, TakeValue(factory_.CreateStatementStorage())) {}

  [[nodiscard]] StatementTemporaryStorage& storage() noexcept {
    if (!storage_.has_value()) {
      std::terminate();
    }
    return *storage_;
  }
  [[nodiscard]] test::WritePagerFixedVfs& vfs() noexcept { return vfs_; }
  void ResetStorage() noexcept { storage_.reset(); }

 private:
  [[nodiscard]] static Pager& BeginRead(Pager& pager) {
    const auto begun = pager.BeginRead();
    if (!begun.has_value()) {
      throw std::runtime_error{begun.error().ToString()};
    }
    return pager;
  }

  test::WritePagerFixedVfs vfs_;
  std::unique_ptr<Pager> pager_;
  TemporaryStorageFactory factory_;
  std::optional<StatementTemporaryStorage> storage_;
};

class PosixSorterEnvironment final {
 public:
  explicit PosixSorterEnvironment(ByteCount threshold = ByteCount{512}) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("modern-sqlite-sorter-test-" + std::to_string(suffix));
    std::filesystem::create_directories(directory_);
    database_path_ = directory_ / "main.db";
    const std::ofstream database{database_path_, std::ios::binary};
    if (!database.is_open()) {
      throw std::runtime_error{"failed to create POSIX sorter database"};
    }
    pager_ = TakeValue(Pager::Open(vfs_, database_path_.string()));
    factory_.emplace(
        TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                  TemporaryStorageOptions{
                                                      .mode = TemporaryStoreMode::kFile,
                                                      .sorter_memory_threshold = threshold,
                                                  })));
    storage_.emplace(TakeValue(factory_->CreateStatementStorage()));
  }

  PosixSorterEnvironment(const PosixSorterEnvironment&) = delete;
  PosixSorterEnvironment& operator=(const PosixSorterEnvironment&) = delete;

  ~PosixSorterEnvironment() noexcept {
    storage_.reset();
    factory_.reset();
    pager_.reset();
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] StatementTemporaryStorage& storage() {
    if (!storage_.has_value()) {
      throw std::runtime_error{"POSIX sorter storage is uninitialized"};
    }
    return *storage_;
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path database_path_;
  NativeVfs vfs_;
  std::unique_ptr<Pager> pager_;
  std::optional<TemporaryStorageFactory> factory_;
  std::optional<StatementTemporaryStorage> storage_;
};

[[nodiscard]] ByteBuffer Encode(std::span<const SqlValue> fields) {
  return TakeValue(EncodeRecord(fields));
}

[[nodiscard]] std::string_view TextField(const RecordView& record, std::size_t index) {
  const RecordFieldView field = TakeValue(record.field(index));
  const std::optional<Utf8View> text = field.text_value();
  if (!text.has_value()) {
    throw std::runtime_error{"sorter test expected a text field"};
  }
  return text->bytes();
}

[[nodiscard]] RecordSorterDescriptor TwoKeyDescriptor() {
  return RecordSorterDescriptor{
      .field_count = 3,
      .key_field_count = 2,
      .key_columns =
          {
              IndexColumnOrder{NoCaseCollation(), IndexSortDirection::kAscending,
                               IndexNullPlacement::kLast},
              IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                               IndexNullPlacement::kFirst},
          },
  };
}

[[nodiscard]] RecordSorterDescriptor OneKeyDescriptor() {
  return RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  };
}

[[nodiscard]] EphemeralRelationDescriptor OneKeyRelationDescriptor() {
  return EphemeralRelationDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  };
}

struct BlobRecordSpec {
  std::int64_t key;
  std::size_t payload_size;
};

[[nodiscard]] ByteBuffer BlobRecord(BlobRecordSpec spec) {
  ByteBuffer payload{ByteCount{spec.payload_size}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x5a});
  const std::array<SqlValue, 2> fields{
      SqlValue::Integer(spec.key),
      SqlValue::Blob(std::move(payload)),
  };
  return Encode(fields);
}

[[nodiscard]] ByteBuffer BlobRecordWithStorageSize(std::int64_t key, std::size_t storage_size) {
  for (std::size_t payload_size = storage_size; payload_size > 0U; --payload_size) {
    ByteBuffer record = BlobRecord({.key = key, .payload_size = payload_size});
    if (record.size().value() + SqliteVarintLength(record.size().value()).value() == storage_size) {
      return record;
    }
  }
  throw std::runtime_error{"no blob record fits the requested PMA storage size"};
}

TEST(RecordSorterApi, ExposesStableStatesAndMoveOnlyOwnership) {
  EXPECT_EQ("writing", RecordSorterStateName(RecordSorterState::kWriting));
  EXPECT_EQ("positioned", RecordSorterStateName(RecordSorterState::kPositioned));
  EXPECT_EQ("exhausted", RecordSorterStateName(RecordSorterState::kExhausted));
  EXPECT_EQ("closed", RecordSorterStateName(RecordSorterState::kClosed));
  EXPECT_EQ("unknown",
            RecordSorterStateName(static_cast<RecordSorterState>(255)));  // NOLINT

  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));
  EXPECT_TRUE(sorter.valid());
  EXPECT_EQ(RecordSorterState::kWriting, sorter.state());
  EXPECT_EQ(0U, sorter.record_count());
  EXPECT_EQ(ByteCount{0}, sorter.memory_usage());

  RecordSorter moved = std::move(sorter);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(sorter.valid());
  EXPECT_EQ(RecordSorterState::kClosed, sorter.state());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(moved.valid());
  moved.Close();
  EXPECT_FALSE(moved.valid());
  EXPECT_EQ(RecordSorterState::kClosed, moved.state());
  moved.Close();
}

TEST(RecordSorterApi, KeepsBorrowedStatementStateStableAcrossStorageMove) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  RecordSorter sorter = TakeValue(storage.CreateRecordSorter(OneKeyDescriptor()));
  const StatementTemporaryStorage moved{std::move(storage)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(storage.valid());

  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
  EXPECT_TRUE(sorter.has_spilled());
  EXPECT_EQ(1U, moved.live_file_count());
  sorter.Close();
}

TEST(RecordSorter, StablySortsOwnedRecordsByConfiguredKeyPrefix) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));

  std::vector<SqlValue> b_two;
  b_two.push_back(SqlValue::Text("b"));
  b_two.push_back(SqlValue::Integer(2));
  b_two.push_back(SqlValue::Text("b-two"));
  std::vector<SqlValue> first_a_two;
  first_a_two.push_back(SqlValue::Text("A"));
  first_a_two.push_back(SqlValue::Integer(2));
  first_a_two.push_back(SqlValue::Text("first-a-two"));
  std::vector<SqlValue> a_one;
  a_one.push_back(SqlValue::Text("a"));
  a_one.push_back(SqlValue::Integer(1));
  a_one.push_back(SqlValue::Text("a-one"));
  std::vector<SqlValue> second_a_two;
  second_a_two.push_back(SqlValue::Text("a"));
  second_a_two.push_back(SqlValue::Integer(2));
  second_a_two.push_back(SqlValue::Text("second-a-two"));
  std::vector<SqlValue> null_key;
  null_key.emplace_back();
  null_key.push_back(SqlValue::Integer(0));
  null_key.push_back(SqlValue::Text("null"));

  EXPECT_TRUE(sorter.Insert(Encode(b_two)).has_value());
  EXPECT_TRUE(sorter.Insert(Encode(first_a_two)).has_value());
  EXPECT_TRUE(sorter.Insert(Encode(a_one)).has_value());
  EXPECT_TRUE(sorter.Insert(Encode(second_a_two)).has_value());
  EXPECT_TRUE(sorter.Insert(Encode(null_key)).has_value());
  EXPECT_EQ(5U, sorter.record_count());
  EXPECT_GT(sorter.memory_usage(), ByteCount{0});

  EXPECT_TRUE(sorter.Rewind().has_value());
  const std::array<std::string_view, 5> expected{
      "first-a-two", "second-a-two", "a-one", "b-two", "null",
  };
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ASSERT_EQ(RecordSorterState::kPositioned, sorter.state());
    const RecordView current = TakeValue(sorter.current_record());
    EXPECT_EQ(expected[index], TextField(current, 2));
    const bool positioned = TakeValue(sorter.Next());
    EXPECT_EQ(index + 1U < expected.size(), positioned);
  }
  EXPECT_EQ(RecordSorterState::kExhausted, sorter.state());
}

TEST(RecordSorter, ComparesCurrentPrefixAndIgnoresPayloadTail) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));
  std::vector<SqlValue> stored;
  stored.push_back(SqlValue::Text("key"));
  stored.push_back(SqlValue::Integer(7));
  stored.push_back(SqlValue::Text("stored"));
  EXPECT_TRUE(sorter.Insert(Encode(stored)).has_value());
  EXPECT_TRUE(sorter.Rewind().has_value());

  std::vector<SqlValue> equivalent;
  equivalent.push_back(SqlValue::Text("KEY"));
  equivalent.push_back(SqlValue::Integer(7));
  equivalent.push_back(SqlValue::Text("other"));
  const ByteBuffer equivalent_bytes = Encode(equivalent);
  const RecordView equivalent_record = TakeValue(RecordView::Parse(equivalent_bytes.view()));
  const auto comparison = sorter.CompareCurrent(equivalent_record);
  ASSERT_TRUE(comparison.has_value());
  EXPECT_EQ(std::weak_ordering::equivalent, *comparison);
}

TEST(RecordSorter, PreservesStableTiesAcrossBottomUpMergeRuns) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));

  constexpr std::size_t kRecordCount = 33;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    std::vector<SqlValue> fields;
    fields.push_back(SqlValue::Text("same"));
    fields.push_back(SqlValue::Integer(7));
    fields.push_back(SqlValue::Integer(static_cast<std::int64_t>(index)));
    ASSERT_TRUE(sorter.Insert(Encode(fields)).has_value());
  }

  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const RecordView current = TakeValue(sorter.current_record());
    const RecordFieldView payload = TakeValue(current.field(2));
    EXPECT_EQ(static_cast<std::int64_t>(index), payload.integer_value());
    EXPECT_EQ(index + 1U < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, DirectlyEncodesValuesIntoTheMemoryOwner) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  const std::array<SqlValue, 2> first{
      SqlValue::Integer(2),
      SqlValue::Text("second"),
  };
  const std::array<SqlValue, 2> second{
      SqlValue::Integer(1),
      SqlValue::Text("first"),
  };

  ASSERT_TRUE(sorter.Insert(first).has_value());
  ASSERT_TRUE(sorter.Insert(second).has_value());
  EXPECT_EQ(2U, sorter.record_count());
  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_EQ("first", TextField(TakeValue(sorter.current_record()), 1));
  ASSERT_TRUE(TakeValue(sorter.Next()));
  EXPECT_EQ("second", TextField(TakeValue(sorter.current_record()), 1));
  EXPECT_FALSE(TakeValue(sorter.Next()));
}

TEST(RecordSorter, GrowsBulkArenaGeometricallyAndPreservesWideStableRecords) {
  constexpr std::size_t kFieldCount = 33;
  constexpr std::size_t kRecordCount = 256;
  SorterEnvironment environment{ByteCount{1U << 16U}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(RecordSorterDescriptor{
      .field_count = kFieldCount,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  }));

  std::size_t growth_count = 0;
  ByteCount previous_usage{0};
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    std::vector<SqlValue> fields;
    fields.reserve(kFieldCount);
    fields.push_back(SqlValue::Integer(1));
    fields.push_back(SqlValue::Integer(static_cast<std::int64_t>(index)));
    while (fields.size() < kFieldCount) {
      fields.push_back(SqlValue::Integer(static_cast<std::int64_t>(fields.size())));
    }
    ASSERT_TRUE(sorter.Insert(fields).has_value());
    if (sorter.memory_usage() != previous_usage) {
      ++growth_count;
      previous_usage = sorter.memory_usage();
    }
  }

  EXPECT_FALSE(sorter.has_spilled());
  EXPECT_LE(growth_count, 8U);
  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const RecordView current = TakeValue(sorter.current_record());
    EXPECT_EQ(static_cast<std::int64_t>(index), TakeValue(current.field(1)).integer_value());
    EXPECT_EQ(index + 1U < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, KeepsGeometricGrowthAfterCrossingAMultiOwnerFairShare) {
  constexpr std::size_t kRecordCount = 1000;
  SorterEnvironment environment{ByteCount{1U << 16U}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter sibling = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));

  std::size_t growth_count = 0;
  ByteCount previous_usage{0};
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const std::array<SqlValue, 2> fields{
        SqlValue::Integer(static_cast<std::int64_t>(kRecordCount - index)),
        SqlValue::Integer(static_cast<std::int64_t>(index)),
    };
    ASSERT_TRUE(sorter.Insert(fields).has_value());
    if (sorter.memory_usage() != previous_usage) {
      ++growth_count;
      previous_usage = sorter.memory_usage();
    }
  }

  EXPECT_LE(growth_count, 12U);
  EXPECT_FALSE(sorter.has_spilled());
  EXPECT_FALSE(sibling.has_spilled());
}

TEST(RecordSorter, EnforcesLifecycleAndResetSemantics) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));

  EXPECT_FALSE(sorter.current_record().has_value());
  EXPECT_FALSE(sorter.Next().has_value());
  EXPECT_TRUE(sorter.Rewind().has_value());
  EXPECT_EQ(RecordSorterState::kExhausted, sorter.state());
  EXPECT_FALSE(sorter.current_record().has_value());
  EXPECT_FALSE(sorter.Next().has_value());

  EXPECT_TRUE(sorter.Reset().has_value());
  EXPECT_EQ(RecordSorterState::kWriting, sorter.state());
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Text("x"));
  fields.push_back(SqlValue::Integer(1));
  fields.push_back(SqlValue::Text("payload"));
  EXPECT_TRUE(sorter.Insert(Encode(fields)).has_value());
  EXPECT_TRUE(sorter.Rewind().has_value());
  EXPECT_FALSE(sorter.Insert(Encode(fields)).has_value());
  EXPECT_FALSE(sorter.Rewind().has_value());
  EXPECT_TRUE(sorter.Reset().has_value());
  EXPECT_EQ(0U, sorter.record_count());
  EXPECT_EQ(ByteCount{0}, sorter.memory_usage());

  sorter.Close();
  EXPECT_FALSE(sorter.Reset().has_value());
  EXPECT_FALSE(sorter.Insert(Encode(fields)).has_value());
}

TEST(RecordSorter, RejectsMalformedWrongShapeAndOverBudgetRecordsWithoutMutation) {
  SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));

  const auto malformed = sorter.Insert(ByteBuffer{});
  ASSERT_FALSE(malformed.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, malformed.error().code());

  std::vector<SqlValue> short_fields;
  short_fields.push_back(SqlValue::Text("x"));
  short_fields.push_back(SqlValue::Integer(1));
  const auto wrong_shape = sorter.Insert(Encode(short_fields));
  ASSERT_FALSE(wrong_shape.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, wrong_shape.error().code());
  EXPECT_EQ(0U, sorter.record_count());

  SorterEnvironment tiny_environment{ByteCount{1}};
  RecordSorter tiny = TakeValue(tiny_environment.storage().CreateRecordSorter(TwoKeyDescriptor()));
  std::vector<SqlValue> valid_fields;
  valid_fields.push_back(SqlValue::Text("x"));
  valid_fields.push_back(SqlValue::Integer(1));
  valid_fields.push_back(SqlValue::Text("payload"));
  const auto over_budget = tiny.Insert(Encode(valid_fields));
  ASSERT_FALSE(over_budget.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, over_budget.error().code());
  EXPECT_EQ(0U, tiny.record_count());
  EXPECT_EQ(ByteCount{0}, tiny.memory_usage());
}

TEST(RecordSorter, RejectsInvalidDescriptorsBeforeAllocatingRuntimeState) {
  SorterEnvironment environment;
  const std::array invalid{
      RecordSorterDescriptor{},
      RecordSorterDescriptor{
          .field_count = 1,
          .key_field_count = 0,
      },
      RecordSorterDescriptor{
          .field_count = kMaximumRecordFieldCount + 1U,
          .key_field_count = 1,
          .key_columns =
              {
                  IndexColumnOrder{BinaryCollation()},
              },
      },
      RecordSorterDescriptor{
          .field_count = 1,
          .key_field_count = 2,
          .key_columns =
              {
                  IndexColumnOrder{BinaryCollation()},
                  IndexColumnOrder{BinaryCollation()},
              },
      },
      RecordSorterDescriptor{
          .field_count = 2,
          .key_field_count = 1,
      },
      RecordSorterDescriptor{
          .field_count = 1,
          .key_field_count = 1,
          .key_columns =
              {
                  IndexColumnOrder{BinaryCollation(), InvalidEnumValue<IndexSortDirection>(2),
                                   IndexNullPlacement::kFirst},
              },
      },
      RecordSorterDescriptor{
          .field_count = 1,
          .key_field_count = 1,
          .key_columns =
              {
                  IndexColumnOrder{BinaryCollation()},
              },
          .record_options =
              RecordCodecOptions{
                  .schema_format = InvalidEnumValue<RecordSchemaFormat>(0),
              },
      },
  };

  for (const RecordSorterDescriptor& descriptor : invalid) {
    const auto rejected = environment.storage().CreateRecordSorter(descriptor);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  }
}

TEST(RecordSorter, SpillsBeforeThresholdCrossingAndAppendsTwoPmasAtRewind) {
  SorterEnvironment environment{ByteCount{400}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(TwoKeyDescriptor()));

  const std::string first_payload(180U, 'f');
  const std::string second_payload(180U, 's');
  std::vector<SqlValue> first;
  first.push_back(SqlValue::Text("b"));
  first.push_back(SqlValue::Integer(2));
  first.push_back(SqlValue::Text(first_payload));
  std::vector<SqlValue> second;
  second.push_back(SqlValue::Text("a"));
  second.push_back(SqlValue::Integer(1));
  second.push_back(SqlValue::Text(second_payload));

  ASSERT_TRUE(sorter.Insert(Encode(first)).has_value());
  EXPECT_FALSE(sorter.has_spilled());
  EXPECT_EQ(0U, environment.vfs().pathless_open_count());
  ASSERT_TRUE(sorter.Insert(Encode(second)).has_value());
  EXPECT_TRUE(sorter.has_spilled());
  EXPECT_EQ(1U, sorter.spilled_run_count());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_TRUE(environment.vfs().pathless_file_present());

  const ByteView first_pma = environment.vfs().pathless_file_bytes();
  const auto size = DecodeSqliteVarint(first_pma);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(first_pma.size() - size->bytes_consumed.value(), size->value);
  const auto stored = RecordView::Parse(
      first_pma.subspan(size->bytes_consumed.value(), static_cast<std::size_t>(size->value)));
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(first_payload, TextField(*stored, 2));

  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_EQ(2U, sorter.spilled_run_count());
  EXPECT_EQ(0U, sorter.merge_level_count());
  EXPECT_EQ(second_payload, TextField(TakeValue(sorter.current_record()), 2));
  EXPECT_TRUE(TakeValue(sorter.Next()));
  EXPECT_EQ(first_payload, TextField(TakeValue(sorter.current_record()), 2));
  EXPECT_FALSE(TakeValue(sorter.Next()));
  EXPECT_EQ(ByteCount{0}, sorter.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());

  sorter.Close();
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(RecordSorter, SupportsRepeatedPageSizedPmasThroughDirectInsertion) {
  SorterEnvironment environment{ByteCount{test::kWritePagerPageSize}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  constexpr std::int64_t kRecordCount = 64;
  for (std::int64_t key = kRecordCount; key > 0; --key) {
    ByteBuffer payload{ByteCount{180}};
    std::ranges::fill(payload.mutable_view(), std::byte{0x5a});
    const std::array<SqlValue, 2> fields{
        SqlValue::Integer(key),
        SqlValue::Blob(std::move(payload)),
    };
    ASSERT_TRUE(sorter.Insert(fields).has_value());
  }

  EXPECT_TRUE(sorter.has_spilled());
  EXPECT_TRUE(sorter.spilled_run_count() > 1U || sorter.merge_level_count() > 0U);
  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::int64_t key = 1; key <= kRecordCount; ++key) {
    EXPECT_EQ(key, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
    EXPECT_EQ(key < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, AvoidsPublishedRunMergeWhenSelfSpillCreatesExactHeadroom) {
  SorterEnvironment environment{ByteCount{test::kWritePagerPageSize}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  constexpr std::int64_t kRecordCount = 19;
  for (std::int64_t key = kRecordCount; key > 0; --key) {
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = key, .payload_size = 32U})).has_value());
  }

  EXPECT_GE(sorter.spilled_run_count(), 3U);
  EXPECT_EQ(0U, sorter.merge_level_count());
  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::int64_t key = 1; key <= kRecordCount; ++key) {
    EXPECT_EQ(key, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
    EXPECT_EQ(key < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, PreflightsDirectPmaMetadataAtTinyThresholds) {
  SorterEnvironment probe{ByteCount{128}, TemporaryStoreMode::kFile};
  RecordSorter probe_sorter = TakeValue(probe.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(probe_sorter.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
  const std::size_t metadata_bytes = probe_sorter.memory_usage().value();
  ASSERT_GT(metadata_bytes, 1U);
  probe_sorter.Close();

  SorterEnvironment below{ByteCount{metadata_bytes - 1U}, TemporaryStoreMode::kFile};
  RecordSorter rejected = TakeValue(below.storage().CreateRecordSorter(OneKeyDescriptor()));
  const auto failed = rejected.Insert(BlobRecord({.key = 1, .payload_size = 900U}));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, failed.error().code());
  EXPECT_FALSE(rejected.valid());
  EXPECT_EQ(0U, below.vfs().pathless_open_count());
  EXPECT_EQ(0U, below.vfs().total_subjournal_writes());

  for (const std::size_t threshold : {metadata_bytes, metadata_bytes + 1U}) {
    SCOPED_TRACE(threshold);
    SorterEnvironment accepted{ByteCount{threshold}, TemporaryStoreMode::kFile};
    RecordSorter sorter = TakeValue(accepted.storage().CreateRecordSorter(OneKeyDescriptor()));
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
    EXPECT_TRUE(sorter.has_spilled());
    EXPECT_EQ(ByteCount{metadata_bytes}, sorter.memory_usage());
    EXPECT_EQ(1U, accepted.vfs().pathless_open_count());
  }
}

TEST(RecordSorter, ExcludesRequesterDuringBulkGrowthAndRetriesAfterSelfPressure) {
  SorterEnvironment environment{ByteCount{512}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  constexpr std::int64_t kRecordCount = 40;
  for (std::int64_t key = kRecordCount; key > 0; --key) {
    ByteBuffer payload{ByteCount{180}};
    std::ranges::fill(payload.mutable_view(), std::byte{0x6b});
    const std::array<SqlValue, 2> fields{
        SqlValue::Integer(key),
        SqlValue::Blob(std::move(payload)),
    };
    ASSERT_TRUE(sorter.Insert(fields).has_value());
  }

  EXPECT_TRUE(sorter.has_spilled());
  EXPECT_GT(sorter.merge_level_count(), 0U);
  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::int64_t key = 1; key <= kRecordCount; ++key) {
    EXPECT_EQ(key, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
    EXPECT_EQ(key < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, BuffersPmaIoByPageInsteadOfRecord) {
  constexpr std::int64_t kRows = 512;
  SorterEnvironment environment{ByteCount{16384}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  for (std::int64_t key = kRows; key > 0; --key) {
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = key, .payload_size = 32U})).has_value());
  }
  ASSERT_TRUE(sorter.Rewind().has_value());
  const std::size_t page_call_bound =
      1U + environment.vfs().pathless_file_bytes().size() / test::kWritePagerPageSize +
      sorter.spilled_run_count();
  std::int64_t expected = 1;
  do {
    const RecordView record = TakeValue(sorter.current_record());
    EXPECT_EQ(expected, TakeValue(record.field(0)).integer_value());
    ++expected;
  } while (TakeValue(sorter.Next()));
  EXPECT_EQ(kRows + 1, expected);
  EXPECT_LE(environment.vfs().total_subjournal_writes(), page_call_bound);
  EXPECT_LE(environment.vfs().total_pathless_reads(), page_call_bound);
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(RecordSorter, ClipsBufferedIoToMidPageSharedExtents) {
  SorterEnvironment environment{ByteCount{64}, TemporaryStoreMode::kFile};
  RecordSorter first = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter second = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(first.Insert(BlobRecord({.key = 1, .payload_size = 31U})).has_value());
  const ByteBuffer first_extent = ByteBuffer::CopyOf(environment.vfs().pathless_file_bytes());
  ASSERT_LT(first_extent.size().value(), test::kWritePagerPageSize);
  ASSERT_TRUE(second.Insert(BlobRecord({.key = 2, .payload_size = 900U})).has_value());
  const std::size_t extent_end = environment.vfs().pathless_file_bytes().size();
  EXPECT_TRUE(std::ranges::equal(first_extent.view(), environment.vfs().pathless_file_bytes().first(
                                                          first_extent.size().value())));

  ASSERT_TRUE(first.Rewind().has_value());
  const auto first_read = environment.vfs().last_pathless_read();
  ASSERT_TRUE(first_read.has_value());
  EXPECT_EQ(FileOffset{0}, first_read->first);
  EXPECT_EQ(first_extent.size(), first_read->second);
  const RecordView first_record = TakeValue(first.current_record());
  EXPECT_GE(first.memory_usage().value(),
            test::kWritePagerPageSize + first_record.encoded().size());

  ASSERT_TRUE(second.Rewind().has_value());
  const auto second_read = environment.vfs().last_pathless_read();
  ASSERT_TRUE(second_read.has_value());
  EXPECT_GE(second_read->first.value(), first_extent.size().value());
  EXPECT_EQ(extent_end, second_read->first.value() + second_read->second.value());
  EXPECT_EQ(1, TakeValue(first_record.field(0)).integer_value());
  EXPECT_EQ(2, TakeValue(TakeValue(second.current_record()).field(0)).integer_value());
  EXPECT_EQ(first.memory_usage().value() + second.memory_usage().value(),
            environment.storage().retained_bytes().value());
  EXPECT_FALSE(TakeValue(first.Next()));
  EXPECT_GT(environment.storage().retained_bytes(), ByteCount{0});
  EXPECT_FALSE(TakeValue(second.Next()));
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(RecordSorter, ReadsVarintsAndPayloadsAcrossPhysicalPageBoundaries) {
  SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ByteBuffer first = BlobRecordWithStorageSize(1, test::kWritePagerPageSize - 1U);
  const auto first_blob =
      TakeValue(TakeValue(RecordView::Parse(first.view())).field(1)).blob_value();
  ASSERT_TRUE(first_blob.has_value());
  const std::size_t first_payload = first_blob->size();
  ASSERT_TRUE(sorter.Insert(std::move(first)).has_value());
  ASSERT_EQ(test::kWritePagerPageSize - 1U, environment.vfs().pathless_file_bytes().size());
  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 2, .payload_size = 900U})).has_value());
  ASSERT_TRUE(sorter.Rewind().has_value());

  const std::array<std::size_t, 2> payload_sizes{first_payload, 900U};
  for (std::size_t index = 0; index < payload_sizes.size(); ++index) {
    const RecordView record = TakeValue(sorter.current_record());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), TakeValue(record.field(0)).integer_value());
    const auto payload = TakeValue(record.field(1)).blob_value();
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload_sizes[index], payload->size());
    EXPECT_TRUE(
        std::ranges::all_of(*payload, [](std::byte byte) { return byte == std::byte{0x5a}; }));
    EXPECT_EQ(index + 1U < payload_sizes.size(), TakeValue(sorter.Next()));
  }
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(RecordSorter, FlushesPageSizedAndOversizedRecordsBeforeExactExtentEof) {
  const std::array<std::size_t, 3> storage_sizes{
      test::kWritePagerPageSize,
      test::kWritePagerPageSize * 2U,
      test::kWritePagerPageSize * 2U + 1U,
  };
  for (const std::size_t storage_size : storage_sizes) {
    SCOPED_TRACE(storage_size);
    SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
    RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
    ASSERT_TRUE(sorter.Insert(BlobRecordWithStorageSize(1, storage_size)).has_value());
    EXPECT_EQ(storage_size, environment.vfs().pathless_file_bytes().size());
    const std::size_t page_calls = 1U + (storage_size - 1U) / test::kWritePagerPageSize;
    EXPECT_EQ(page_calls, environment.vfs().total_subjournal_writes());
    ASSERT_TRUE(sorter.Rewind().has_value());
    EXPECT_EQ(1, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
    EXPECT_EQ(page_calls, environment.vfs().total_pathless_reads());
    EXPECT_FALSE(TakeValue(sorter.Next()));
    EXPECT_EQ(page_calls, environment.vfs().total_pathless_reads());
    EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
  }
}

TEST(RecordSorter, PoisonsStatementOnFullOrFinalPmaFlushFailure) {
  for (std::size_t successful_flushes = 0; successful_flushes < 3U; ++successful_flushes) {
    SCOPED_TRACE(successful_flushes);
    SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
    RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
    environment.vfs().FailPathlessWriteAfter(successful_flushes, ErrorCode::kIo);
    const auto failed =
        sorter.Insert(BlobRecordWithStorageSize(1, test::kWritePagerPageSize * 2U + 1U));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_FALSE(sorter.valid());
    EXPECT_TRUE(environment.storage().failed());
    EXPECT_EQ(successful_flushes, environment.vfs().total_subjournal_writes());
    EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
    EXPECT_TRUE(environment.vfs().pathless_file_present());
    environment.ResetStorage();
    EXPECT_FALSE(environment.vfs().pathless_file_present());
  }
}

TEST(RecordSorter, RejectsShortPmaRefillsWithoutPublishingARecord) {
  const std::array<std::pair<std::size_t, ByteCount>, 3> faults{
      std::pair{0U, ByteCount{1}},
      std::pair{1U, ByteCount{1}},
      std::pair{2U, ByteCount{0}},
  };
  for (const auto& [successful_refills, maximum_bytes] : faults) {
    SCOPED_TRACE(successful_refills);
    SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
    RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
    ASSERT_TRUE(sorter.Insert(BlobRecordWithStorageSize(1, test::kWritePagerPageSize * 2U + 1U))
                    .has_value());
    environment.vfs().ShortPathlessReadAfter(successful_refills, maximum_bytes);
    const auto failed = sorter.Rewind();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, failed.error().code());
    EXPECT_FALSE(sorter.valid());
    EXPECT_TRUE(environment.storage().failed());
    EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
  }
}

TEST(RecordSorter, RejectsMalformedPmaHeadersPayloadsAndFieldShapes) {
  enum class CorruptionKind : std::uint8_t {
    TruncatedVarint,
    PayloadPastEnd,
    RecordHeader,
    FieldCount,
  };
  const std::array kinds{
      CorruptionKind::TruncatedVarint,
      CorruptionKind::PayloadPastEnd,
      CorruptionKind::RecordHeader,
      CorruptionKind::FieldCount,
  };
  for (const CorruptionKind kind : kinds) {
    SCOPED_TRACE(static_cast<int>(kind));
    SorterEnvironment environment{ByteCount{40}, TemporaryStoreMode::kFile};
    RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
    const std::array<SqlValue, 2> fields{SqlValue::Integer(0), SqlValue::Integer(0)};
    ASSERT_TRUE(sorter.Insert(Encode(fields)).has_value());
    MutableByteView bytes = environment.vfs().mutable_pathless_file_bytes();
    switch (kind) {
      case CorruptionKind::TruncatedVarint:
        std::ranges::fill(bytes, std::byte{0x80});
        break;
      case CorruptionKind::PayloadPastEnd:
        bytes[0] = std::byte{0x7f};
        break;
      case CorruptionKind::RecordHeader:
        bytes[1] = std::byte{0};
        break;
      case CorruptionKind::FieldCount: {
        const std::array<SqlValue, 1> one_field{SqlValue::Integer(1)};
        const ByteBuffer record = Encode(one_field);
        ASSERT_LT(record.size().value(), bytes.size());
        bytes[0] = static_cast<std::byte>(record.size().value());
        std::ranges::copy(record.view(), bytes.begin() + 1);
        break;
      }
    }
    const auto failed = sorter.Rewind();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, failed.error().code());
    EXPECT_FALSE(sorter.valid());
    EXPECT_TRUE(environment.storage().failed());
    EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
  }
}

TEST(RecordSorter, ReleasesBufferedReadersOnResetAndCloseBeforeEof) {
  SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  for (std::int64_t key = 2; key >= 1; --key) {
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = key, .payload_size = 900U})).has_value());
  }
  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_GT(environment.storage().retained_bytes(), environment.storage().memory_budget());
  ASSERT_TRUE(sorter.Reset().has_value());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 3, .payload_size = 900U})).has_value());
  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_EQ(3, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
  sorter.Close();
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(RecordSorter, SharesOneLazyStatementPmaFileAndReusesReleasedExtents) {
  SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
  RecordSorter first = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter second = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));

  ASSERT_TRUE(first.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
  ASSERT_TRUE(second.Insert(BlobRecord({.key = 2, .payload_size = 900U})).has_value());
  EXPECT_TRUE(first.has_spilled());
  EXPECT_TRUE(second.has_spilled());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());
  EXPECT_EQ(first.memory_usage().value() + second.memory_usage().value(),
            environment.storage().retained_bytes().value());

  first.Close();
  second.Close();
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
  const std::size_t peak_file_size = environment.vfs().pathless_file_bytes().size();

  RecordSorter reused = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(reused.Insert(BlobRecord({.key = 3, .payload_size = 900U})).has_value());
  EXPECT_TRUE(reused.has_spilled());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_LE(environment.vfs().pathless_file_bytes().size(), peak_file_size);
  reused.Close();

  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(RecordSorter, CoalescesFreeExtentsAndChoosesTheLowestOffsetFit) {
  SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
  RecordSorter first = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter second = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter tail = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(first.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
  ASSERT_TRUE(second.Insert(BlobRecord({.key = 2, .payload_size = 900U})).has_value());
  ASSERT_TRUE(tail.Insert(BlobRecord({.key = 3, .payload_size = 900U})).has_value());
  const std::size_t high_water = environment.vfs().pathless_file_bytes().size();

  first.Close();
  second.Close();
  RecordSorter reused = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(reused.Insert(BlobRecord({.key = 4, .payload_size = 1500U})).has_value());
  EXPECT_EQ(high_water, environment.vfs().pathless_file_bytes().size());

  const ByteView bytes = environment.vfs().pathless_file_bytes();
  const auto size = DecodeSqliteVarint(bytes);
  ASSERT_TRUE(size.has_value());
  const auto record = RecordView::Parse(
      bytes.subspan(size->bytes_consumed.value(), static_cast<std::size_t>(size->value)));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(4, TakeValue(record->field(0)).integer_value());
}

TEST(RecordSorter, SpillsLargestSorterUnderStatementMemoryPressure) {
  SorterEnvironment environment{ByteCount{512}, TemporaryStoreMode::kFile};
  RecordSorter first = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter second = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));

  ASSERT_TRUE(first.Insert(BlobRecord({.key = 1, .payload_size = 200U})).has_value());
  ASSERT_TRUE(second.Insert(BlobRecord({.key = 2, .payload_size = 16U})).has_value());
  EXPECT_FALSE(first.has_spilled());
  EXPECT_FALSE(second.has_spilled());
  ASSERT_TRUE(second.Insert(BlobRecord({.key = 3, .payload_size = 250U})).has_value());
  EXPECT_TRUE(first.has_spilled());
  EXPECT_FALSE(second.has_spilled());
  EXPECT_EQ(first.memory_usage().value() + second.memory_usage().value(),
            environment.storage().retained_bytes().value());
}

TEST(RecordSorter, DoesNotSpillSparseArenasWhenALaterSorterOpens) {
  SorterEnvironment environment{ByteCount{1024}, TemporaryStoreMode::kFile};
  RecordSorter first = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter second = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  const std::array<SqlValue, 2> first_values{
      SqlValue::Integer(1),
      SqlValue::Integer(1),
  };
  const std::array<SqlValue, 2> second_values{
      SqlValue::Integer(2),
      SqlValue::Integer(2),
  };
  ASSERT_TRUE(first.Insert(first_values).has_value());
  ASSERT_TRUE(second.Insert(second_values).has_value());

  RecordSorter third = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  const std::array<SqlValue, 2> third_values{
      SqlValue::Integer(3),
      SqlValue::Integer(3),
  };
  ASSERT_TRUE(third.Insert(third_values).has_value());
  EXPECT_FALSE(first.has_spilled());
  EXPECT_FALSE(second.has_spilled());
  EXPECT_FALSE(third.has_spilled());
  EXPECT_EQ(
      first.memory_usage().value() + second.memory_usage().value() + third.memory_usage().value(),
      environment.storage().retained_bytes().value());
}

TEST(RecordSorter, PreservesTieOrderWhenOversizedRecordFollowsMemoryRun) {
  SorterEnvironment environment{ByteCount{256}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 1, .payload_size = 16U})).has_value());
  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 1, .payload_size = 900U})).has_value());
  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_GT(sorter.memory_usage(), environment.storage().memory_budget());
  EXPECT_EQ(sorter.memory_usage(), environment.storage().retained_bytes());

  RecordView current = TakeValue(sorter.current_record());
  RecordFieldView payload = TakeValue(current.field(1));
  std::optional<ByteView> blob = payload.blob_value();
  ASSERT_TRUE(blob.has_value());
  EXPECT_EQ(16U, blob->size());
  EXPECT_TRUE(TakeValue(sorter.Next()));
  current = TakeValue(sorter.current_record());
  payload = TakeValue(current.field(1));
  blob = payload.blob_value();
  ASSERT_TRUE(blob.has_value());
  EXPECT_EQ(900U, blob->size());
  EXPECT_FALSE(TakeValue(sorter.Next()));
  EXPECT_EQ(ByteCount{0}, sorter.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(RecordSorter, NeverPressureSpillsAPositionedSibling) {
  SorterEnvironment environment{ByteCount{700}, TemporaryStoreMode::kFile};
  RecordSorter positioned = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  RecordSorter writer = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));

  ASSERT_TRUE(positioned.Insert(BlobRecord({.key = 1, .payload_size = 200U})).has_value());
  ASSERT_TRUE(positioned.Rewind().has_value());
  const RecordView current = TakeValue(positioned.current_record());
  ASSERT_TRUE(writer.Insert(BlobRecord({.key = 2, .payload_size = 16U})).has_value());
  ASSERT_TRUE(writer.Insert(BlobRecord({.key = 3, .payload_size = 250U})).has_value());

  EXPECT_EQ(1, TakeValue(current.field(0)).integer_value());
  EXPECT_EQ(RecordSorterState::kPositioned, positioned.state());
  EXPECT_TRUE(writer.has_spilled());
}

TEST(RecordSorter, CoexistsWithRelationArenaWithinTwoStatementFiles) {
  SorterEnvironment environment{ByteCount{128}, TemporaryStoreMode::kFile};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(OneKeyRelationDescriptor()));
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));

  ASSERT_TRUE(
      relation
          .Insert(BlobRecord({.key = 1, .payload_size = 900U}), EphemeralInsertMode::kKeepExisting)
          .has_value());
  ASSERT_TRUE(sorter.Insert(BlobRecord({.key = 2, .payload_size = 900U})).has_value());
  EXPECT_TRUE(relation.file_backed());
  EXPECT_TRUE(sorter.has_spilled());
  EXPECT_EQ(2U, environment.storage().live_file_count());
  EXPECT_EQ(2U, environment.storage().peak_live_file_count());
  EXPECT_EQ(2U, environment.vfs().pathless_open_count());

  relation.Close();
  sorter.Close();
  EXPECT_EQ(2U, environment.storage().live_file_count());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(RecordSorter, BuildsFanInSixteenMergeLevelsAndStreamsSortedRecords) {
  PosixSorterEnvironment environment{ByteCount{2048}};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{BinaryCollation()},
          },
  }));

  constexpr std::size_t kRecordCount = 1000;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    std::array<SqlValue, 2> fields{
        SqlValue::Integer(static_cast<std::int64_t>(kRecordCount - index)),
        SqlValue::Integer(static_cast<std::int64_t>(index)),
    };
    ASSERT_TRUE(sorter.Insert(Encode(fields)).has_value());
  }
  ASSERT_TRUE(sorter.Rewind().has_value());
  EXPECT_GT(sorter.merge_level_count(), 0U);
  EXPECT_LE(sorter.spilled_run_count(), 16U);

  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const RecordView current = TakeValue(sorter.current_record());
    const RecordFieldView key = TakeValue(current.field(0));
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), key.integer_value());
    EXPECT_EQ(index + 1U < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, PreservesStableTiesAcrossPmaRuns) {
  PosixSorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(RecordSorterDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              IndexColumnOrder{NoCaseCollation()},
          },
  }));

  constexpr std::size_t kRecordCount = 34;
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    std::array<SqlValue, 2> fields{
        SqlValue::Text(index % 2U == 0U ? "same" : "SAME"),
        SqlValue::Integer(static_cast<std::int64_t>(index)),
    };
    ASSERT_TRUE(sorter.Insert(Encode(fields)).has_value());
  }
  ASSERT_TRUE(sorter.Rewind().has_value());
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    const RecordView current = TakeValue(sorter.current_record());
    const RecordFieldView payload = TakeValue(current.field(1));
    EXPECT_EQ(static_cast<std::int64_t>(index), payload.integer_value());
    EXPECT_EQ(index + 1U < kRecordCount, TakeValue(sorter.Next()));
  }
}

TEST(RecordSorter, PoisonsStatementAndDefersFileCleanupAfterSpillFailures) {
  SorterEnvironment write_failure{ByteCount{400}, TemporaryStoreMode::kFile};
  RecordSorter writer = TakeValue(write_failure.storage().CreateRecordSorter(TwoKeyDescriptor()));
  RecordSorter sibling = TakeValue(write_failure.storage().CreateRecordSorter(TwoKeyDescriptor()));
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Text("x"));
  fields.push_back(SqlValue::Integer(1));
  fields.push_back(SqlValue::Text(std::string(180U, 'p')));
  ASSERT_TRUE(writer.Insert(Encode(fields)).has_value());
  write_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const auto failed_write = writer.Insert(Encode(fields));
  ASSERT_FALSE(failed_write.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_write.error().code());
  EXPECT_FALSE(writer.valid());
  EXPECT_TRUE(write_failure.storage().failed());
  const auto sibling_after_failure = sibling.Insert(Encode(fields));
  ASSERT_FALSE(sibling_after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, sibling_after_failure.error().code());
  EXPECT_FALSE(sibling.valid());
  EXPECT_TRUE(write_failure.vfs().pathless_file_present());
  write_failure.ResetStorage();
  EXPECT_FALSE(write_failure.vfs().pathless_file_present());

  SorterEnvironment read_failure{ByteCount{400}, TemporaryStoreMode::kFile};
  RecordSorter reader = TakeValue(read_failure.storage().CreateRecordSorter(TwoKeyDescriptor()));
  ASSERT_TRUE(reader.Insert(Encode(fields)).has_value());
  ASSERT_TRUE(reader.Insert(Encode(fields)).has_value());
  read_failure.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);
  const auto failed_read = reader.Rewind();
  ASSERT_FALSE(failed_read.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_read.error().code());
  EXPECT_FALSE(reader.valid());
  EXPECT_TRUE(read_failure.storage().failed());
  EXPECT_TRUE(read_failure.vfs().pathless_file_present());
  read_failure.ResetStorage();
  EXPECT_FALSE(read_failure.vfs().pathless_file_present());
}

TEST(RecordSorter, PreservesInputExtentsUntilMergedOutputPublication) {
  SorterEnvironment environment{ByteCount{2048}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  for (std::int64_t key = 20; key >= 1; --key) {
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = key, .payload_size = 3000U})).has_value());
  }
  ASSERT_GT(sorter.spilled_run_count(), 16U);
  const ByteView before = environment.vfs().pathless_file_bytes();
  const std::vector<std::byte> input_bytes(before.begin(), before.end());

  environment.vfs().FailPathlessWriteAfter(1, ErrorCode::kIo);
  const auto failed = sorter.Rewind();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(sorter.valid());
  EXPECT_TRUE(environment.storage().failed());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  const ByteView after = environment.vfs().pathless_file_bytes();
  ASSERT_GE(after.size(), input_bytes.size());
  EXPECT_TRUE(std::ranges::equal(input_bytes, after.first(input_bytes.size())));
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(RecordSorter, PoisonsStatementOnLateSharedPmaReadFailure) {
  SorterEnvironment environment{ByteCount{4096}, TemporaryStoreMode::kFile};
  RecordSorter sorter = TakeValue(environment.storage().CreateRecordSorter(OneKeyDescriptor()));
  for (std::int64_t key = 8; key >= 1; --key) {
    ASSERT_TRUE(sorter.Insert(BlobRecord({.key = key, .payload_size = 200U})).has_value());
  }
  TemporaryMemoryAccount pressure = TakeValue(environment.storage().RegisterMemoryOwner());
  const ByteCount pressure_bytes{environment.storage().memory_budget().value() - 64U};
  ASSERT_TRUE(pressure.Reserve(pressure_bytes).has_value());
  ASSERT_TRUE(pressure.Release(pressure_bytes).has_value());
  ASSERT_EQ(1U, sorter.spilled_run_count());
  ASSERT_TRUE(sorter.Rewind().has_value());
  const std::size_t reads_before = environment.vfs().total_pathless_reads();
  environment.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);

  ASSERT_TRUE(TakeValue(sorter.Next()));
  EXPECT_EQ(2, TakeValue(TakeValue(sorter.current_record()).field(0)).integer_value());
  EXPECT_EQ(reads_before, environment.vfs().total_pathless_reads());
  const auto failed = sorter.Next();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(sorter.valid());
  EXPECT_TRUE(environment.storage().failed());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

}  // namespace
}  // namespace modern_sqlite
