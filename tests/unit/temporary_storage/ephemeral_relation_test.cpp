#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
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

static_assert(!std::is_copy_constructible_v<EphemeralRelation>);
static_assert(std::is_nothrow_move_constructible_v<EphemeralRelation>);

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

class RelationEnvironment final {
 public:
  explicit RelationEnvironment(TemporaryStoreMode mode)
      : pager_(TakeValue(
            Pager::Open(vfs_, test::kWritePagerInputPath,
                        PagerOptions{
                            .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                            .cache_capacity_pages = 1,
                        }))),
        factory_(TakeValue(TemporaryStorageFactory::Create(vfs_, *pager_,
                                                           TemporaryStorageOptions{
                                                               .mode = mode,
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

[[nodiscard]] EphemeralRelationDescriptor Descriptor() {
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

[[nodiscard]] ByteBuffer IntegerKey(std::int64_t key) {
  const std::array<SqlValue, 1> values{SqlValue::Integer(key)};
  return Encode(values);
}

[[nodiscard]] ByteBuffer RealKey(double key) {
  const std::array<SqlValue, 1> values{SqlValue::Real(key)};
  return Encode(values);
}

[[nodiscard]] ByteBuffer IntegerRow(std::int64_t key, std::int64_t payload) {
  const std::array<SqlValue, 2> values{
      SqlValue::Integer(key),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

[[nodiscard]] ByteBuffer RealRow(double key, std::int64_t payload) {
  const std::array<SqlValue, 2> values{
      SqlValue::Real(key),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

[[nodiscard]] ByteBuffer ValueKey(SqlValue key) {
  std::array<SqlValue, 1> values{std::move(key)};
  return Encode(values);
}

[[nodiscard]] ByteBuffer ValueRow(SqlValue key, std::int64_t payload) {
  std::array<SqlValue, 2> values{
      std::move(key),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

struct BlobRowSpec {
  std::int64_t key;
  std::size_t payload_size;
};

[[nodiscard]] ByteBuffer BlobRow(BlobRowSpec spec) {
  ByteBuffer payload{ByteCount{spec.payload_size}};
  std::ranges::fill(payload.mutable_view(),
                    static_cast<std::byte>(static_cast<std::uint8_t>(spec.key)));
  std::array<SqlValue, 2> values{
      SqlValue::Integer(spec.key),
      SqlValue::Blob(std::move(payload)),
  };
  return Encode(values);
}

[[nodiscard]] ByteBuffer MultiKey(std::optional<std::string_view> text, std::int64_t number) {
  const std::array<SqlValue, 2> values{
      text.has_value() ? SqlValue::Text(std::string{*text}) : SqlValue{},
      SqlValue::Integer(number),
  };
  return Encode(values);
}

[[nodiscard]] ByteBuffer MultiRow(std::optional<std::string_view> text, std::int64_t number,
                                  std::int64_t payload) {
  const std::array<SqlValue, 3> values{
      text.has_value() ? SqlValue::Text(std::string{*text}) : SqlValue{},
      SqlValue::Integer(number),
      SqlValue::Integer(payload),
  };
  return Encode(values);
}

[[nodiscard]] SqlValue Field(const RecordView& record, std::size_t index) {
  return TakeValue(record.field(index)).ToOwned();
}

TEST(EphemeralRelationApi, PublishesMoveOnlyLifecycleAndInsertResults) {
  EXPECT_EQ("writing", EphemeralRelationStateName(EphemeralRelationState::kWriting));
  EXPECT_EQ("positioned", EphemeralRelationStateName(EphemeralRelationState::kPositioned));
  EXPECT_EQ("exhausted", EphemeralRelationStateName(EphemeralRelationState::kExhausted));
  EXPECT_EQ("closed", EphemeralRelationStateName(EphemeralRelationState::kClosed));
  EXPECT_EQ("unknown",
            EphemeralRelationStateName(static_cast<EphemeralRelationState>(255)));  // NOLINT

  const RelationEnvironment environment{TemporaryStoreMode::kMemory};
  EphemeralRelation relation =
      TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
  EXPECT_TRUE(relation.valid());
  EXPECT_EQ(EphemeralRelationState::kWriting, relation.state());
  EXPECT_EQ(0U, relation.record_count());
  EXPECT_FALSE(relation.file_backed());

  EphemeralRelation moved = std::move(relation);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(relation.valid());
  EXPECT_EQ(EphemeralRelationState::kClosed, relation.state());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  moved.Close();
  EXPECT_FALSE(moved.valid());
}

TEST(EphemeralRelation, KeepsReplacesFindsErasesAndIteratesInBothBackends) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(mode == TemporaryStoreMode::kMemory ? "memory" : "file");
    RelationEnvironment environment{mode};
    EphemeralRelation relation =
        TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, relation.file_backed());
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, environment.vfs().pathless_file_present());

    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(IntegerRow(2, 20), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kDuplicate,
              TakeValue(relation.Insert(RealRow(2.0, 21), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kReplaced,
              TakeValue(relation.Insert(RealRow(2.0, 22), EphemeralInsertMode::kReplaceExisting)));
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(IntegerRow(1, 10), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(IntegerRow(3, 30), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(3U, relation.record_count());

    EXPECT_TRUE(TakeValue(relation.Contains(IntegerKey(1).view())));
    EXPECT_TRUE(TakeValue(relation.Contains(RealKey(2.0).view())));
    EXPECT_FALSE(TakeValue(relation.Contains(IntegerKey(4).view())));
    EXPECT_TRUE(TakeValue(relation.Erase(IntegerKey(1).view())));
    EXPECT_FALSE(TakeValue(relation.Erase(IntegerKey(1).view())));
    EXPECT_EQ(2U, relation.record_count());

    RequireStatus(relation.Rewind());
    ASSERT_EQ(EphemeralRelationState::kPositioned, relation.state());
    RecordView current = TakeValue(relation.current_record());
    EXPECT_EQ(SqlValueType::kReal, Field(current, 0).type());
    EXPECT_EQ(2.0, Field(current, 0).real_value());
    EXPECT_EQ(22, Field(current, 1).integer_value());
    EXPECT_TRUE(TakeValue(relation.Next()));
    current = TakeValue(relation.current_record());
    EXPECT_EQ(3, Field(current, 0).integer_value());
    EXPECT_EQ(30, Field(current, 1).integer_value());
    EXPECT_FALSE(TakeValue(relation.Next()));
    EXPECT_EQ(EphemeralRelationState::kExhausted, relation.state());

    EXPECT_FALSE(
        relation.Insert(IntegerRow(4, 40), EphemeralInsertMode::kKeepExisting).has_value());
    EXPECT_FALSE(relation.Contains(IntegerKey(2).view()).has_value());
    EXPECT_FALSE(relation.Erase(IntegerKey(2).view()).has_value());
    EXPECT_TRUE(relation.valid());
    RequireStatus(relation.Reset());
    EXPECT_EQ(EphemeralRelationState::kWriting, relation.state());
    EXPECT_EQ(0U, relation.record_count());
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, environment.vfs().pathless_file_present());
    RequireStatus(relation.Rewind());
    EXPECT_EQ(EphemeralRelationState::kExhausted, relation.state());

    relation.Close();
    EXPECT_FALSE(environment.vfs().pathless_file_present());
  }
}

TEST(EphemeralRelation, HonorsMultiColumnDirectionNullAndRtrimMetadata) {
  const EphemeralRelationDescriptor descriptor{
      .field_count = 3,
      .key_field_count = 2,
      .key_columns =
          {
              IndexColumnOrder{RTrimCollation(), IndexSortDirection::kAscending,
                               IndexNullPlacement::kLast},
              IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                               IndexNullPlacement::kFirst},
          },
  };
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(mode == TemporaryStoreMode::kMemory ? "memory" : "file");
    const RelationEnvironment environment{mode};
    EphemeralRelation relation =
        TakeValue(environment.factory().CreateEphemeralRelation(descriptor));
    EXPECT_EQ(
        EphemeralInsertResult::kInserted,
        TakeValue(relation.Insert(MultiRow("x ", 1, 10), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kDuplicate,
              TakeValue(relation.Insert(MultiRow("x", 1, 11), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(
        EphemeralInsertResult::kReplaced,
        TakeValue(relation.Insert(MultiRow("x", 1, 12), EphemeralInsertMode::kReplaceExisting)));
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(MultiRow("x", 2, 20), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(MultiRow(std::nullopt, 0, 30),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_TRUE(TakeValue(relation.Contains(MultiKey("x   ", 1).view())));

    RequireStatus(relation.Rewind());
    const std::array<std::int64_t, 3> expected_payloads{20, 12, 30};
    for (std::size_t index = 0; index < expected_payloads.size(); ++index) {
      const RecordView current = TakeValue(relation.current_record());
      EXPECT_EQ(expected_payloads[index], Field(current, 2).integer_value());
      EXPECT_EQ(index + 1U != expected_payloads.size(), TakeValue(relation.Next()));
    }
  }
}

TEST(EphemeralRelation, MatchesSqliteKeyEqualityAndRepresentativeRules) {
  EphemeralRelationDescriptor descriptor = Descriptor();
  descriptor.key_columns[0] = IndexColumnOrder{NoCaseCollation()};

  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(mode == TemporaryStoreMode::kMemory ? "memory" : "file");
    const RelationEnvironment environment{mode};
    EphemeralRelation relation =
        TakeValue(environment.factory().CreateEphemeralRelation(descriptor));

    EXPECT_EQ(
        EphemeralInsertResult::kInserted,
        TakeValue(relation.Insert(ValueRow(SqlValue{}, 1), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(
        EphemeralInsertResult::kDuplicate,
        TakeValue(relation.Insert(ValueRow(SqlValue{}, 2), EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(
        EphemeralInsertResult::kReplaced,
        TakeValue(relation.Insert(ValueRow(SqlValue{}, 3), EphemeralInsertMode::kReplaceExisting)));

    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(ValueRow(SqlValue::Integer(1), 10),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kDuplicate,
              TakeValue(relation.Insert(ValueRow(SqlValue::Real(1.0), 11),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kReplaced,
              TakeValue(relation.Insert(ValueRow(SqlValue::Real(1.0), 12),
                                        EphemeralInsertMode::kReplaceExisting)));

    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(ValueRow(SqlValue::Text("A"), 20),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kDuplicate,
              TakeValue(relation.Insert(ValueRow(SqlValue::Text("a"), 21),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(EphemeralInsertResult::kReplaced,
              TakeValue(relation.Insert(ValueRow(SqlValue::Text("a"), 22),
                                        EphemeralInsertMode::kReplaceExisting)));

    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(ValueRow(SqlValue::Text("1"), 30),
                                        EphemeralInsertMode::kKeepExisting)));
    ByteBuffer blob{ByteCount{1}};
    blob.mutable_view()[0] = std::byte{0x41};
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(ValueRow(SqlValue::Blob(std::move(blob)), 40),
                                        EphemeralInsertMode::kKeepExisting)));
    EXPECT_EQ(5U, relation.record_count());

    EXPECT_TRUE(TakeValue(relation.Contains(ValueKey(SqlValue{}).view())));
    EXPECT_TRUE(TakeValue(relation.Contains(ValueKey(SqlValue::Integer(1)).view())));
    EXPECT_TRUE(TakeValue(relation.Contains(ValueKey(SqlValue::Text("A")).view())));

    RequireStatus(relation.Rewind());
    const std::array expected_types{
        SqlValueType::kNull, SqlValueType::kReal, SqlValueType::kText,
        SqlValueType::kText, SqlValueType::kBlob,
    };
    const std::array<std::int64_t, 5> expected_payloads{3, 12, 30, 22, 40};
    for (std::size_t index = 0; index < expected_types.size(); ++index) {
      const RecordView current = TakeValue(relation.current_record());
      EXPECT_EQ(expected_types[index], Field(current, 0).type());
      EXPECT_EQ(expected_payloads[index], Field(current, 1).integer_value());
      EXPECT_EQ(index + 1U != expected_types.size(), TakeValue(relation.Next()));
    }
  }
}

TEST(EphemeralRelation, FileBackendSplitsRebalancesClearsAndRemovesItsFile) {
  RelationEnvironment environment{TemporaryStoreMode::kFile};
  EphemeralRelation relation =
      TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
  for (std::int64_t key = 127; key >= 0; --key) {
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(BlobRow(BlobRowSpec{
                                            .key = key,
                                            .payload_size = key % 17 == 0 ? 900U : 80U,
                                        }),
                                        EphemeralInsertMode::kKeepExisting)));
  }
  for (std::int64_t key = 0; key < 128; key += 2) {
    EXPECT_TRUE(TakeValue(relation.Erase(IntegerKey(key).view())));
  }
  EXPECT_EQ(64U, relation.record_count());
  EXPECT_GT(environment.vfs().total_subjournal_writes(), 0U);

  RequireStatus(relation.Rewind());
  for (std::int64_t expected = 1; expected < 128; expected += 2) {
    const RecordView current = TakeValue(relation.current_record());
    EXPECT_EQ(expected, Field(current, 0).integer_value());
    const SqlValue payload_value = Field(current, 1);
    const std::optional<ByteView> payload = payload_value.blob_value();
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(expected % 17 == 0 ? 900U : 80U, payload->size());
    EXPECT_EQ(expected + 2 < 128, TakeValue(relation.Next()));
  }

  RequireStatus(relation.Reset());
  EXPECT_EQ(0U, relation.record_count());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(relation.Insert(IntegerRow(9, 90), EphemeralInsertMode::kKeepExisting)));
  RequireStatus(relation.Rewind());
  EXPECT_EQ(9, Field(TakeValue(relation.current_record()), 0).integer_value());
  relation.Close();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(EphemeralRelation, RejectsInvalidShapesAndClosesOnMalformedRecords) {
  const RelationEnvironment environment{TemporaryStoreMode::kMemory};
  const auto zero_fields =
      environment.factory().CreateEphemeralRelation(EphemeralRelationDescriptor{
          .field_count = 0,
          .key_field_count = 1,
          .key_columns = {IndexColumnOrder{BinaryCollation()}},
      });
  const auto mismatched_key =
      environment.factory().CreateEphemeralRelation(EphemeralRelationDescriptor{
          .field_count = 2,
          .key_field_count = 1,
      });
  ASSERT_FALSE(zero_fields.has_value());
  ASSERT_FALSE(mismatched_key.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero_fields.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, mismatched_key.error().code());

  EphemeralRelation malformed =
      TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
  ByteBuffer invalid_record{ByteCount{1}};
  invalid_record.mutable_view()[0] = std::byte{0xff};
  const auto inserted =
      malformed.Insert(std::move(invalid_record), EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(inserted.has_value());
  EXPECT_FALSE(malformed.valid());

  EphemeralRelation wrong_key =
      TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
  const auto contained = wrong_key.Contains(IntegerRow(1, 10).view());
  ASSERT_FALSE(contained.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, contained.error().code());
  EXPECT_FALSE(wrong_key.valid());

  EphemeralRelation lifecycle =
      TakeValue(environment.factory().CreateEphemeralRelation(Descriptor()));
  EXPECT_FALSE(lifecycle.current_record().has_value());
  EXPECT_FALSE(lifecycle.Next().has_value());
  const auto invalid_mode =
      lifecycle.Insert(IntegerRow(1, 10), static_cast<EphemeralInsertMode>(255));  // NOLINT
  ASSERT_FALSE(invalid_mode.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_mode.error().code());
  EXPECT_TRUE(lifecycle.valid());
  lifecycle.Close();
  EXPECT_FALSE(lifecycle.Reset().has_value());
}

TEST(EphemeralRelation, FileBackendPropagatesIoFailuresAndCleansUp) {
  RelationEnvironment open_failure{TemporaryStoreMode::kFile};
  open_failure.vfs().FailNextPathlessOpen(ErrorCode::kIo);
  const auto failed_open = open_failure.factory().CreateEphemeralRelation(Descriptor());
  ASSERT_FALSE(failed_open.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_open.error().code());
  EXPECT_FALSE(open_failure.vfs().pathless_file_present());

  RelationEnvironment write_failure{TemporaryStoreMode::kFile};
  EphemeralRelation writing =
      TakeValue(write_failure.factory().CreateEphemeralRelation(Descriptor()));
  write_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const auto failed_insert = writing.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                            EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(failed_insert.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_insert.error().code());
  EXPECT_FALSE(writing.valid());
  EXPECT_FALSE(write_failure.vfs().pathless_file_present());

  RelationEnvironment read_failure{TemporaryStoreMode::kFile};
  EphemeralRelation reading =
      TakeValue(read_failure.factory().CreateEphemeralRelation(Descriptor()));
  for (std::int64_t key = 1; key <= 4; ++key) {
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(reading.Insert(BlobRow(BlobRowSpec{.key = key, .payload_size = 900U}),
                                       EphemeralInsertMode::kKeepExisting)));
  }
  read_failure.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);
  const Status failed_rewind = reading.Rewind();
  ASSERT_FALSE(failed_rewind.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_rewind.error().code());
  EXPECT_FALSE(reading.valid());
  EXPECT_FALSE(read_failure.vfs().pathless_file_present());

  RelationEnvironment late_read_failure{TemporaryStoreMode::kFile};
  EphemeralRelation draining =
      TakeValue(late_read_failure.factory().CreateEphemeralRelation(Descriptor()));
  constexpr std::int64_t kDrainRows = 24;
  for (std::int64_t key = 0; key < kDrainRows; ++key) {
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(draining.Insert(BlobRow(BlobRowSpec{.key = key, .payload_size = 900U}),
                                        EphemeralInsertMode::kKeepExisting)));
  }
  RequireStatus(draining.Rewind());
  late_read_failure.vfs().FailPathlessReadAfter(0, ErrorCode::kIo);
  std::size_t rows_seen = 1;
  Result<bool> advanced = true;
  while (advanced.has_value() && *advanced) {
    advanced = draining.Next();
    if (advanced.has_value() && *advanced) {
      ++rows_seen;
    }
  }
  ASSERT_FALSE(advanced.has_value());
  EXPECT_EQ(ErrorCode::kIo, advanced.error().code());
  EXPECT_GT(rows_seen, 0U);
  EXPECT_LT(rows_seen, static_cast<std::size_t>(kDrainRows));
  EXPECT_FALSE(draining.valid());
  EXPECT_FALSE(late_read_failure.vfs().pathless_file_present());

  RelationEnvironment reset_failure{TemporaryStoreMode::kFile};
  EphemeralRelation resetting =
      TakeValue(reset_failure.factory().CreateEphemeralRelation(Descriptor()));
  for (std::int64_t key = 0; key < 8; ++key) {
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(resetting.Insert(BlobRow(BlobRowSpec{.key = key, .payload_size = 900U}),
                                         EphemeralInsertMode::kKeepExisting)));
  }
  reset_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const Status failed_reset = resetting.Reset();
  ASSERT_FALSE(failed_reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_reset.error().code());
  EXPECT_FALSE(resetting.valid());
  EXPECT_FALSE(reset_failure.vfs().pathless_file_present());
}

}  // namespace
}  // namespace modern_sqlite
