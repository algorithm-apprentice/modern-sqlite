#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
  explicit SorterEnvironment(ByteCount threshold = ByteCount{1U << 20U})
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

TEST(RecordSorterApi, ExposesStableStatesAndMoveOnlyOwnership) {
  EXPECT_EQ("writing", RecordSorterStateName(RecordSorterState::kWriting));
  EXPECT_EQ("positioned", RecordSorterStateName(RecordSorterState::kPositioned));
  EXPECT_EQ("exhausted", RecordSorterStateName(RecordSorterState::kExhausted));
  EXPECT_EQ("closed", RecordSorterStateName(RecordSorterState::kClosed));
  EXPECT_EQ("unknown",
            RecordSorterStateName(static_cast<RecordSorterState>(255)));  // NOLINT

  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));
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

TEST(RecordSorter, StablySortsOwnedRecordsByConfiguredKeyPrefix) {
  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));

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
  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));
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
  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));

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

TEST(RecordSorter, EnforcesLifecycleAndResetSemantics) {
  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));

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
  const SorterEnvironment environment;
  RecordSorter sorter = TakeValue(environment.factory().CreateRecordSorter(TwoKeyDescriptor()));

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

  const SorterEnvironment tiny_environment{ByteCount{1}};
  RecordSorter tiny = TakeValue(tiny_environment.factory().CreateRecordSorter(TwoKeyDescriptor()));
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
  const SorterEnvironment environment;
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
    const auto rejected = environment.factory().CreateRecordSorter(descriptor);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  }
}

}  // namespace
}  // namespace modern_sqlite
