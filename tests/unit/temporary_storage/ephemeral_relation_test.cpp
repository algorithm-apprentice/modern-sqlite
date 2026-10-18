#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "src/storage/btree/writer_internal.hpp"
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
  explicit RelationEnvironment(TemporaryStoreMode mode,
                               std::optional<ByteCount> memory_threshold = std::nullopt)
      : pager_(TakeValue(
            Pager::Open(vfs_, test::kWritePagerInputPath,
                        PagerOptions{
                            .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                            .cache_capacity_pages = 1,
                        }))),
        factory_(TakeValue(
            TemporaryStorageFactory::Create(vfs_, *pager_,
                                            TemporaryStorageOptions{
                                                .mode = mode,
                                                .sorter_memory_threshold = memory_threshold,
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

[[nodiscard]] EphemeralRelationDescriptor LookupDescriptor() {
  EphemeralRelationDescriptor descriptor = Descriptor();
  descriptor.key_field_count = descriptor.field_count;
  descriptor.key_columns.emplace_back(BinaryCollation());
  descriptor.purpose = EphemeralRelationPurpose::kLookup;
  return descriptor;
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

[[nodiscard]] ByteBuffer TextBlobRow(std::string key, std::size_t payload_size) {
  ByteBuffer payload{ByteCount{payload_size}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x5a});
  std::array<SqlValue, 2> values{
      SqlValue::Text(std::move(key)),
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

  RelationEnvironment environment{TemporaryStoreMode::kMemory};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
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

TEST(EphemeralRelationApi, KeepsBorrowedStatementStateStableAcrossStorageMove) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager =
      TakeValue(Pager::Open(vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{test::kWritePagerPageSize},
                                .cache_capacity_pages = 1,
                            }));
  RequireStatus(pager->BeginRead());
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{128},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  EphemeralRelation relation = TakeValue(storage.CreateEphemeralRelation(Descriptor()));
  const StatementTemporaryStorage moved{std::move(storage)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(storage.valid());

  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(relation.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                      EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(relation.file_backed());
  EXPECT_EQ(1U, moved.live_file_count());
  relation.Close();
}

TEST(EphemeralRelation, KeepsReplacesFindsErasesAndIteratesInBothBackends) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(mode == TemporaryStoreMode::kMemory ? "memory" : "file");
    RelationEnvironment environment{
        mode, mode == TemporaryStoreMode::kFile ? std::optional{ByteCount{64}} : std::nullopt};
    EphemeralRelation relation =
        TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
    EXPECT_FALSE(relation.file_backed());
    EXPECT_FALSE(environment.vfs().pathless_file_present());

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
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, relation.file_backed());
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, environment.vfs().pathless_file_present());
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
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, environment.vfs().pathless_file_present());
    environment.ResetStorage();
    EXPECT_FALSE(environment.vfs().pathless_file_present());
  }
}

TEST(EphemeralRelation, PreservesPositionedRowsWhileAnotherSharedRootIsBuilt) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{1}};
  EphemeralRelation first = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EphemeralRelation second = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  ASSERT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(first.Insert(IntegerRow(1, 10), EphemeralInsertMode::kKeepExisting)));
  RequireStatus(first.Rewind());
  const RecordView current = TakeValue(first.current_record());
  ASSERT_EQ(10, Field(current, 1).integer_value());
  ASSERT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(IntegerRow(2, 20), EphemeralInsertMode::kKeepExisting)));
  EXPECT_EQ(10, Field(current, 1).integer_value());
  EXPECT_EQ(10, Field(TakeValue(first.current_record()), 1).integer_value());
  EXPECT_FALSE(TakeValue(first.Next()));
  EXPECT_EQ(1U, environment.storage().live_file_count());
}

TEST(EphemeralRelation, ReprobesTheFirstEquivalentPrefixInBothBackends) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(mode == TemporaryStoreMode::kMemory ? "memory" : "file");
    RelationEnvironment environment{mode, ByteCount{4096}};
    EphemeralRelation relation =
        TakeValue(environment.storage().CreateEphemeralRelation(LookupDescriptor()));
    const std::array<SqlValue, 1> first_key{SqlValue::Integer(1)};
    EXPECT_FALSE(relation.SeekPrefix(first_key).has_value());
    RequireStatus(relation.ClearPosition());
    EXPECT_EQ(EphemeralRelationState::kWriting, relation.state());
    for (const auto& [key, payload] :
         {std::pair{1, 20}, std::pair{2, 30}, std::pair{1, 10}, std::pair{1, 15}}) {
      EXPECT_EQ(
          EphemeralInsertResult::kInserted,
          TakeValue(relation.Insert(IntegerRow(key, payload), EphemeralInsertMode::kKeepExisting)));
    }
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, relation.file_backed());
    EXPECT_FALSE(relation.Rewind().has_value());
    RequireStatus(relation.SealForLookup());
    EXPECT_EQ(EphemeralRelationState::kLookupReady, relation.state());
    EXPECT_FALSE(relation.current_record().has_value());
    EXPECT_FALSE(relation.Contains(IntegerKey(1).view()).has_value());
    EXPECT_FALSE(
        relation.Insert(IntegerRow(3, 40), EphemeralInsertMode::kKeepExisting).has_value());
    ASSERT_TRUE(TakeValue(relation.SeekPrefix(first_key)));
    EXPECT_EQ(EphemeralRelationState::kLookupPositioned, relation.state());
    EXPECT_EQ(10, Field(TakeValue(relation.current_record()), 1).integer_value());
    ASSERT_TRUE(TakeValue(relation.NextPrefix(first_key)));
    EXPECT_EQ(15, Field(TakeValue(relation.current_record()), 1).integer_value());
    ASSERT_TRUE(TakeValue(relation.NextPrefix(first_key)));
    EXPECT_EQ(20, Field(TakeValue(relation.current_record()), 1).integer_value());
    EXPECT_FALSE(TakeValue(relation.NextPrefix(first_key)));
    EXPECT_EQ(EphemeralRelationState::kLookupReady, relation.state());
    EXPECT_FALSE(relation.current_record().has_value());
    const std::array<SqlValue, 1> missing{SqlValue::Integer(0)};
    EXPECT_FALSE(TakeValue(relation.SeekPrefix(missing)));
    const std::array<SqlValue, 2> complete{SqlValue::Integer(1), SqlValue::Integer(20)};
    ASSERT_TRUE(TakeValue(relation.SeekPrefix(complete)));
    EXPECT_EQ(20, Field(TakeValue(relation.current_record()), 1).integer_value());
    EXPECT_FALSE(TakeValue(relation.NextPrefix(complete)));
    ASSERT_TRUE(TakeValue(relation.SeekPrefix(first_key)));
    RequireStatus(relation.ClearPosition());
    EXPECT_EQ(EphemeralRelationState::kLookupReady, relation.state());
    RequireStatus(relation.Reset());
    EXPECT_EQ(EphemeralRelationState::kWriting, relation.state());
    EXPECT_EQ(0U, relation.record_count());
    RequireStatus(relation.SealForLookup());
    EXPECT_FALSE(TakeValue(relation.SeekPrefix(first_key)));
  }
}

TEST(EphemeralRelation, BuildsLookupRecordsDirectlyInTheSharedFileArena) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{4096}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(LookupDescriptor()));
  EXPECT_FALSE(relation.file_backed());
  EXPECT_EQ(0U, environment.storage().live_file_count());
  for (std::int64_t key = 32; key != 0; --key) {
    ASSERT_EQ(
        EphemeralInsertResult::kInserted,
        TakeValue(relation.Insert(IntegerRow(key, key * 10), EphemeralInsertMode::kKeepExisting)));
    EXPECT_TRUE(relation.file_backed());
    EXPECT_EQ(ByteCount{0}, relation.memory_usage());
  }
  EXPECT_EQ(ByteCount{0}, environment.storage().peak_retained_bytes());
  EXPECT_EQ(1U, environment.storage().live_file_count());
  RequireStatus(relation.SealForLookup());
  const std::array<SqlValue, 1> key{SqlValue::Integer(17)};
  ASSERT_TRUE(TakeValue(relation.SeekPrefix(key)));
  EXPECT_EQ(170, Field(TakeValue(relation.current_record()), 1).integer_value());
}

TEST(EphemeralRelation, KeepsEmptyLookupLazyAndRejectsInvalidPrefixLifecycles) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{1}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(LookupDescriptor()));
  const std::array<SqlValue, 1> key{SqlValue::Integer(1)};
  EXPECT_FALSE(relation.NextPrefix(key).has_value());
  RequireStatus(relation.SealForLookup());
  EXPECT_FALSE(TakeValue(relation.SeekPrefix(key)));
  EXPECT_FALSE(relation.SeekPrefix({}).has_value());
  const std::array<SqlValue, 3> too_wide{SqlValue::Integer(1), SqlValue::Integer(2),
                                         SqlValue::Integer(3)};
  EXPECT_FALSE(relation.SeekPrefix(too_wide).has_value());
  EXPECT_EQ(EphemeralRelationState::kLookupReady, relation.state());
  EXPECT_FALSE(relation.SealForLookup().has_value());
  EXPECT_FALSE(relation.Rewind().has_value());
  EXPECT_EQ(0U, environment.storage().live_file_count());
  relation.Close();
  EXPECT_FALSE(relation.ClearPosition().has_value());
  EXPECT_FALSE(relation.SeekPrefix(key).has_value());
  EphemeralRelation member = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EXPECT_FALSE(member.SealForLookup().has_value());
  RequireStatus(member.ClearPosition());
  EXPECT_EQ(EphemeralRelationState::kWriting, member.state());
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
    RelationEnvironment environment{mode};
    EphemeralRelation relation =
        TakeValue(environment.storage().CreateEphemeralRelation(descriptor));
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
    RelationEnvironment environment{mode};
    EphemeralRelation relation =
        TakeValue(environment.storage().CreateEphemeralRelation(descriptor));

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
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{1024}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
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
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(EphemeralRelation, SharesOneLazyStatementArenaAcrossFileModeRelations) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{1024}};
  EphemeralRelation first = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EphemeralRelation second = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));

  EXPECT_FALSE(first.file_backed());
  EXPECT_FALSE(second.file_backed());
  EXPECT_FALSE(environment.vfs().pathless_file_present());
  EXPECT_EQ(0U, environment.storage().live_file_count());

  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(first.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                   EphemeralInsertMode::kKeepExisting)));
  EXPECT_FALSE(first.file_backed());
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(first.Insert(BlobRow(BlobRowSpec{.key = 3, .payload_size = 900U}),
                                   EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(first.file_backed());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());

  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 900U}),
                                    EphemeralInsertMode::kKeepExisting)));
  EXPECT_FALSE(second.file_backed());
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(BlobRow(BlobRowSpec{.key = 4, .payload_size = 900U}),
                                    EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(second.file_backed());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());
  EXPECT_TRUE(TakeValue(first.Contains(IntegerKey(1).view())));
  EXPECT_TRUE(TakeValue(second.Contains(IntegerKey(2).view())));

  RequireStatus(first.Rewind());
  const RecordView borrowed = TakeValue(first.current_record());
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(BlobRow(BlobRowSpec{.key = 6, .payload_size = 900U}),
                                    EphemeralInsertMode::kKeepExisting)));
  EXPECT_EQ(1, Field(borrowed, 0).integer_value());

  first.Close();
  second.Close();
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EXPECT_EQ(1U, environment.storage().live_file_count());

  const ByteCount peak_overhead = environment.storage().overhead_bytes();
  EphemeralRelationDescriptor reused_descriptor = Descriptor();
  reused_descriptor.key_columns[0] = IndexColumnOrder{NoCaseCollation()};
  EphemeralRelation reused =
      TakeValue(environment.storage().CreateEphemeralRelation(reused_descriptor));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(reused.Insert(TextBlobRow("A", 900U), EphemeralInsertMode::kKeepExisting)));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(reused.Insert(TextBlobRow("B", 900U), EphemeralInsertMode::kKeepExisting)));
  EXPECT_EQ(EphemeralInsertResult::kDuplicate,
            TakeValue(reused.Insert(TextBlobRow("a", 900U), EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(reused.file_backed());
  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  reused.Close();
  EXPECT_EQ(peak_overhead, environment.storage().overhead_bytes());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(EphemeralRelation, BoundsOneSharedCacheAcrossManyPositionedRoots) {
  constexpr std::size_t kRelationCount = 32;
  constexpr std::size_t kCompletedOperationOverflowPages = 1;
  constexpr std::size_t kSingleLevelRootPinnedPages = 1;
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{16}};
  std::vector<EphemeralRelation> relations;
  relations.reserve(kRelationCount);
  EXPECT_EQ(0U, environment.storage().relation_cache_capacity_pages());
  EXPECT_EQ(0U, environment.storage().relation_cache_peak_allocated_pages());

  for (std::size_t index = 0; index < kRelationCount; ++index) {
    EphemeralRelation relation =
        TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
    const auto key = static_cast<std::int64_t>(index + 1U);
    EXPECT_EQ(
        EphemeralInsertResult::kInserted,
        TakeValue(relation.Insert(IntegerRow(key, key * 10), EphemeralInsertMode::kKeepExisting)));
    RequireStatus(relation.Rewind());
    ASSERT_TRUE(relation.current_record().has_value());
    relations.push_back(std::move(relation));
  }

  EXPECT_EQ(1U, environment.vfs().pathless_open_count());
  EXPECT_EQ(1U, environment.storage().live_file_count());
  EXPECT_EQ(1U, environment.storage().relation_cache_capacity_pages());
  EXPECT_EQ(kRelationCount * kSingleLevelRootPinnedPages,
            environment.storage().relation_cache_pin_count());
  EXPECT_EQ(kRelationCount, environment.storage().relation_cache_page_count());
  EXPECT_EQ(kRelationCount, environment.storage().relation_cache_allocated_pages());
  EXPECT_EQ(kRelationCount + kCompletedOperationOverflowPages,
            environment.storage().relation_cache_peak_allocated_pages());
  EXPECT_LE(environment.storage().relation_cache_pin_count(), kRelationCount * kMaximumBtreeDepth);
  EXPECT_LE(environment.storage().relation_cache_page_count(),
            environment.storage().relation_cache_allocated_pages());
  EXPECT_LE(environment.storage().relation_cache_allocated_pages(),
            environment.storage().relation_cache_capacity_pages() +
                kRelationCount * kMaximumBtreeDepth + kCompletedOperationOverflowPages);
  EXPECT_LE(environment.storage().relation_cache_allocated_pages(),
            environment.storage().relation_cache_peak_allocated_pages());
  EXPECT_LE(environment.storage().relation_cache_peak_allocated_pages(),
            environment.storage().relation_cache_capacity_pages() +
                kRelationCount * kMaximumBtreeDepth + btree_internal::kMaximumMutationPages +
                kCompletedOperationOverflowPages);

  for (EphemeralRelation& relation : relations) {
    relation.Close();
  }
  EXPECT_EQ(0U, environment.storage().relation_cache_pin_count());
}

TEST(EphemeralRelation, AccountsMemoryAndSpillsLargestOwnerUnderStatementPressure) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{512}};
  EphemeralRelation first = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EphemeralRelation second = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));

  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(first.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 200U}),
                                   EphemeralInsertMode::kKeepExisting)));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 16U}),
                                    EphemeralInsertMode::kKeepExisting)));
  EXPECT_GT(first.memory_usage(), second.memory_usage());
  EXPECT_EQ(first.memory_usage().value() + second.memory_usage().value(),
            environment.storage().retained_bytes().value());

  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(second.Insert(BlobRow(BlobRowSpec{.key = 3, .payload_size = 250U}),
                                    EphemeralInsertMode::kKeepExisting)));
  EXPECT_TRUE(first.file_backed());
  EXPECT_FALSE(second.file_backed());
  EXPECT_EQ(ByteCount{0}, first.memory_usage());
  EXPECT_EQ(second.memory_usage(), environment.storage().retained_bytes());
  EXPECT_EQ(1U, environment.storage().live_file_count());

  first.Close();
  second.Close();
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(EphemeralRelation, AccountsCopiedOverflowRecordsOnlyWhilePositioned) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{8192}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  for (std::int64_t key = 1; key <= 2; ++key) {
    EXPECT_EQ(EphemeralInsertResult::kInserted,
              TakeValue(relation.Insert(BlobRow(BlobRowSpec{.key = key, .payload_size = 5000U}),
                                        EphemeralInsertMode::kKeepExisting)));
  }
  EXPECT_TRUE(relation.file_backed());
  EXPECT_EQ(ByteCount{0}, relation.memory_usage());

  RequireStatus(relation.Rewind());
  EXPECT_GT(relation.memory_usage(), ByteCount{0});
  EXPECT_EQ(relation.memory_usage(), environment.storage().retained_bytes());
  EXPECT_TRUE(TakeValue(relation.Next()));
  EXPECT_GT(relation.memory_usage(), ByteCount{0});
  EXPECT_FALSE(TakeValue(relation.Next()));
  EXPECT_EQ(ByteCount{0}, relation.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(EphemeralRelation, TracksAnOversizedPositionedRecordAsAFileWorkingSet) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{128}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(relation.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                      EphemeralInsertMode::kKeepExisting)));
  RequireStatus(relation.Rewind());
  EXPECT_GT(relation.memory_usage(), environment.storage().memory_budget());
  EXPECT_EQ(relation.memory_usage(), environment.storage().retained_bytes());
  EXPECT_FALSE(TakeValue(relation.Next()));
  EXPECT_EQ(ByteCount{0}, relation.memory_usage());
  EXPECT_EQ(ByteCount{0}, environment.storage().retained_bytes());
}

TEST(EphemeralRelation, MemoryModeRejectsOversizedRecordsWithoutLosingExistingRows) {
  RelationEnvironment environment{TemporaryStoreMode::kMemory, ByteCount{128}};
  EphemeralRelation relation =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(relation.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 16U}),
                                      EphemeralInsertMode::kKeepExisting)));
  const ByteCount retained = relation.memory_usage();

  const auto too_large = relation.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 900U}),
                                         EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(too_large.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, too_large.error().code());
  EXPECT_TRUE(relation.valid());
  EXPECT_EQ(1U, relation.record_count());
  EXPECT_EQ(retained, relation.memory_usage());
  EXPECT_EQ(retained, environment.storage().retained_bytes());
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(EphemeralRelation, DefersRetiredRootClearingAndPoisonsSharedArenaOnFailure) {
  RelationEnvironment environment{TemporaryStoreMode::kFile, ByteCount{128}};
  EphemeralRelation first = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  EXPECT_EQ(EphemeralInsertResult::kInserted,
            TakeValue(first.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                   EphemeralInsertMode::kKeepExisting)));
  first.Close();
  EXPECT_FALSE(environment.storage().failed());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  EphemeralRelation sibling =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));

  environment.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  EphemeralRelation reused = TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  const auto failed = reused.Insert(BlobRow(BlobRowSpec{.key = 2, .payload_size = 900U}),
                                    EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_FALSE(reused.valid());
  EXPECT_TRUE(environment.storage().failed());
  EXPECT_TRUE(environment.vfs().pathless_file_present());
  const auto sibling_after_failure = sibling.Contains(IntegerKey(1).view());
  ASSERT_FALSE(sibling_after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, sibling_after_failure.error().code());
  EXPECT_FALSE(sibling.valid());

  const auto after_failure = environment.storage().CreateEphemeralRelation(Descriptor());
  ASSERT_FALSE(after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, after_failure.error().code());
  environment.ResetStorage();
  EXPECT_FALSE(environment.vfs().pathless_file_present());
}

TEST(EphemeralRelation, RejectsInvalidShapesAndClosesOnMalformedRecords) {
  RelationEnvironment environment{TemporaryStoreMode::kMemory};
  const auto zero_fields =
      environment.storage().CreateEphemeralRelation(EphemeralRelationDescriptor{
          .field_count = 0,
          .key_field_count = 1,
          .key_columns = {IndexColumnOrder{BinaryCollation()}},
      });
  const auto mismatched_key =
      environment.storage().CreateEphemeralRelation(EphemeralRelationDescriptor{
          .field_count = 2,
          .key_field_count = 1,
      });
  ASSERT_FALSE(zero_fields.has_value());
  ASSERT_FALSE(mismatched_key.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, zero_fields.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, mismatched_key.error().code());

  EphemeralRelation malformed =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  ByteBuffer invalid_record{ByteCount{1}};
  invalid_record.mutable_view()[0] = std::byte{0xff};
  const auto inserted =
      malformed.Insert(std::move(invalid_record), EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(inserted.has_value());
  EXPECT_FALSE(malformed.valid());

  EphemeralRelation wrong_key =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
  const auto contained = wrong_key.Contains(IntegerRow(1, 10).view());
  ASSERT_FALSE(contained.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, contained.error().code());
  EXPECT_FALSE(wrong_key.valid());

  EphemeralRelation lifecycle =
      TakeValue(environment.storage().CreateEphemeralRelation(Descriptor()));
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
  RelationEnvironment open_failure{TemporaryStoreMode::kFile, ByteCount{128}};
  open_failure.vfs().FailNextPathlessOpen(ErrorCode::kIo);
  EphemeralRelation failed_open =
      TakeValue(open_failure.storage().CreateEphemeralRelation(Descriptor()));
  const auto failed_open_insert = failed_open.Insert(
      BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}), EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(failed_open_insert.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_open_insert.error().code());
  EXPECT_FALSE(open_failure.vfs().pathless_file_present());

  RelationEnvironment write_failure{TemporaryStoreMode::kFile, ByteCount{128}};
  EphemeralRelation writing =
      TakeValue(write_failure.storage().CreateEphemeralRelation(Descriptor()));
  write_failure.vfs().FailPathlessWriteAfter(0, ErrorCode::kIo);
  const auto failed_insert = writing.Insert(BlobRow(BlobRowSpec{.key = 1, .payload_size = 900U}),
                                            EphemeralInsertMode::kKeepExisting);
  ASSERT_FALSE(failed_insert.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed_insert.error().code());
  EXPECT_FALSE(writing.valid());
  EXPECT_TRUE(write_failure.storage().failed());
  EXPECT_TRUE(write_failure.vfs().pathless_file_present());
  write_failure.ResetStorage();
  EXPECT_FALSE(write_failure.vfs().pathless_file_present());

  RelationEnvironment read_failure{TemporaryStoreMode::kFile, ByteCount{1024}};
  EphemeralRelation reading =
      TakeValue(read_failure.storage().CreateEphemeralRelation(Descriptor()));
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
  EXPECT_TRUE(read_failure.storage().failed());
  EXPECT_TRUE(read_failure.vfs().pathless_file_present());
  read_failure.ResetStorage();
  EXPECT_FALSE(read_failure.vfs().pathless_file_present());

  RelationEnvironment late_read_failure{TemporaryStoreMode::kFile, ByteCount{1024}};
  EphemeralRelation draining =
      TakeValue(late_read_failure.storage().CreateEphemeralRelation(Descriptor()));
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
  EXPECT_TRUE(late_read_failure.storage().failed());
  EXPECT_TRUE(late_read_failure.vfs().pathless_file_present());
  late_read_failure.ResetStorage();
  EXPECT_FALSE(late_read_failure.vfs().pathless_file_present());

  RelationEnvironment reset_failure{TemporaryStoreMode::kFile, ByteCount{1024}};
  EphemeralRelation resetting =
      TakeValue(reset_failure.storage().CreateEphemeralRelation(Descriptor()));
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
  EXPECT_TRUE(reset_failure.storage().failed());
  EXPECT_TRUE(reset_failure.vfs().pathless_file_present());
  reset_failure.ResetStorage();
  EXPECT_FALSE(reset_failure.vfs().pathless_file_present());
}

}  // namespace
}  // namespace modern_sqlite
