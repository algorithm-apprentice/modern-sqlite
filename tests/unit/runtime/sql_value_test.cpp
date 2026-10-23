#include "modern_sqlite/runtime/sql_value.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "src/runtime/sql_value_access.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<SqlValue>);
static_assert(!std::is_copy_assignable_v<SqlValue>);
static_assert(std::is_nothrow_move_constructible_v<SqlValue>);
static_assert(std::is_nothrow_move_assignable_v<SqlValue>);

template <typename Source>
concept CanBorrowStable = requires(SqlValue& destination, Source&& source) {
  SqlValueAccess::BorrowStable(destination, std::forward<Source>(source));
};

static_assert(CanBorrowStable<SqlValue&>);
static_assert(CanBorrowStable<const SqlValue&>);
static_assert(!CanBorrowStable<SqlValue>);
static_assert(!CanBorrowStable<const SqlValue>);

template <typename Source>
concept CanBorrowArithmetic = requires(Source&& source, std::optional<SqlValue>& scratch) {
  SqlValueAccess::ArithmeticNumeric(std::forward<Source>(source), scratch);
};

static_assert(CanBorrowArithmetic<SqlValue&>);
static_assert(CanBorrowArithmetic<const SqlValue&>);
static_assert(!CanBorrowArithmetic<SqlValue>);
static_assert(!CanBorrowArithmetic<const SqlValue>);

[[nodiscard]] SqlValue TextValue(std::string_view text) {
  return SqlValue::Text(std::string{text});
}

[[nodiscard]] SqlValue BlobValue(std::string_view bytes) {
  return SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(bytes)));
}

void ExpectNull(const SqlValue& value) { EXPECT_EQ(SqlValueType::kNull, value.type()); }

TEST(SqlValue, BorrowsExistingArithmeticScalarsAndUsesOnlyExplicitConversionScratch) {
  const std::array scalars{SqlValue{}, SqlValue::Integer(7), SqlValue::Real(-0.0)};
  for (const SqlValue& value : scalars) {
    std::optional<SqlValue> scratch;
    const SqlValue& numeric = SqlValueAccess::ArithmeticNumeric(value, scratch);
    EXPECT_EQ(&value, &numeric);
    EXPECT_FALSE(scratch.has_value());
    EXPECT_EQ(value.type(), numeric.type());
  }
  for (const bool blob : {false, true}) {
    const SqlValue value = blob ? BlobValue("1.5tail") : TextValue("1.5tail");
    std::optional<SqlValue> scratch;
    const SqlValue& numeric = SqlValueAccess::ArithmeticNumeric(value, scratch);
    ASSERT_TRUE(scratch.has_value());
    EXPECT_EQ(&*scratch, &numeric);
    ASSERT_EQ(SqlValueType::kReal, numeric.type());
    EXPECT_EQ(1.5, numeric.real_value());
    EXPECT_EQ(blob ? SqlValueType::kBlob : SqlValueType::kText, value.type());
    EXPECT_EQ("1.5tail", blob ? AsStringView(*value.blob_value()) : value.text_value()->bytes());
  }
}

void ExpectInteger(const SqlValue& value, std::int64_t expected) {
  ASSERT_EQ(SqlValueType::kInteger, value.type());
  ASSERT_TRUE(value.integer_value().has_value());
  EXPECT_EQ(expected, *value.integer_value());
}

void ExpectReal(const SqlValue& value, double expected) {
  ASSERT_EQ(SqlValueType::kReal, value.type());
  ASSERT_TRUE(value.real_value().has_value());
  EXPECT_EQ(expected, *value.real_value());
}

void ExpectText(const SqlValue& value, std::string_view expected) {
  ASSERT_EQ(SqlValueType::kText, value.type());
  ASSERT_TRUE(value.text_value().has_value());
  EXPECT_EQ(expected, value.text_value()->bytes());
}

void ExpectBlob(const SqlValue& value, std::string_view expected) {
  ASSERT_EQ(SqlValueType::kBlob, value.type());
  ASSERT_TRUE(value.blob_value().has_value());
  EXPECT_EQ(expected, AsStringView(*value.blob_value()));
}

TEST(SqlValue, RepresentsEveryStorageClassAndPreservesOwnedBytes) {
  const std::string text{"a\0b", 3};
  const std::string blob{"\xff\0z", 3};
  const SqlValue null_value;
  const SqlValue integer_value = SqlValue::Integer(-42);
  const SqlValue real_value = SqlValue::Real(1.25);
  const SqlValue text_value = TextValue(text);
  const SqlValue blob_value = BlobValue(blob);

  ExpectNull(null_value);
  ExpectInteger(integer_value, -42);
  ExpectReal(real_value, 1.25);
  ExpectText(text_value, text);
  ExpectBlob(blob_value, blob);
}

TEST(SqlValue, ClonesTextAndBlobOnlyWhenExplicitlyRequested) {
  const SqlValue text = TextValue("a text payload long enough to avoid small-string storage");
  const SqlValue blob = BlobValue("a blob payload long enough to require vector storage");

  const SqlValue text_clone = text.Clone();
  const SqlValue blob_clone = blob.Clone();

  const auto text_view = text.text_value();
  const auto cloned_text_view = text_clone.text_value();
  const auto blob_view = blob.blob_value();
  const auto cloned_blob_view = blob_clone.blob_value();
  ASSERT_TRUE(text_view.has_value());
  ASSERT_TRUE(cloned_text_view.has_value());
  ASSERT_TRUE(blob_view.has_value());
  ASSERT_TRUE(cloned_blob_view.has_value());
  ExpectText(text_clone, text_view->bytes());
  ExpectBlob(blob_clone, AsStringView(*blob_view));
  EXPECT_NE(text_view->data(), cloned_text_view->data());
  EXPECT_NE(blob_view->data(), cloned_blob_view->data());
}

TEST(SqlValue, ReportsRetainedDynamicStorageCapacity) {
  std::string text;
  text.reserve(1024);
  text = "payload";
  const SqlValue text_value = SqlValue::Text(std::move(text));
  const SqlValue blob_value = BlobValue("blob");

  EXPECT_GE(text_value.owned_capacity_bytes(), 1024U);
  EXPECT_GE(blob_value.owned_capacity_bytes(), 4U);
  EXPECT_EQ(SqlValue::Integer(1).owned_capacity_bytes(), 0U);
}

TEST(SqlValue, OwnedMovesRebindViewsAndLeaveNullSources) {
  for (const std::string_view bytes :
       {"inline", "a heap-owned text payload that does not use small-string storage"}) {
    SqlValue source = TextValue(bytes);
    SqlValue moved{std::move(source)};
    ExpectText(moved, bytes);
    ExpectNull(source);
    source = TextValue("reused");
    ExpectText(moved, bytes);

    SqlValue destination = BlobValue("previous");
    destination = std::move(moved);
    ExpectText(destination, bytes);
    ExpectNull(moved);
    SqlValue& alias = destination;
    destination = std::move(alias);
    ExpectText(destination, bytes);
  }

  constexpr std::string_view blob_bytes{"a\0b", 3};
  SqlValue source = BlobValue(blob_bytes);
  SqlValue moved{std::move(source)};
  ExpectBlob(moved, blob_bytes);
  ExpectNull(source);
  source = SqlValue::Integer(7);
  source = std::move(moved);
  ExpectBlob(source, blob_bytes);
  ExpectNull(moved);
}

TEST(SqlValue, ReusableBytesSurviveScalarNullAndByteTypeTransitions) {
  const std::string original(256, 'x');
  SqlValue value = TextValue("external");
  ASSERT_TRUE(SqlValueAccess::CopyText(value, Utf8View{original}).has_value());
  ExpectText(value, original);
  const auto* address = AsBytes(value.text_value()->bytes()).data();
  const std::size_t capacity = value.owned_capacity_bytes();
  EXPECT_GE(capacity, original.size() + 2U);

  SqlValueAccess::SetInteger(value, 17);
  ExpectInteger(value, 17);
  EXPECT_EQ(capacity, value.owned_capacity_bytes());
  SqlValueAccess::SetReal(value, 1.25);
  ExpectReal(value, 1.25);
  EXPECT_EQ(capacity, value.owned_capacity_bytes());
  SqlValueAccess::SetReal(value, std::numeric_limits<double>::quiet_NaN());
  ExpectNull(value);
  EXPECT_EQ(capacity, value.owned_capacity_bytes());
  SqlValueAccess::SetNull(value);
  EXPECT_EQ(capacity, value.owned_capacity_bytes());

  constexpr std::string_view blob{"b\0c", 3};
  ASSERT_TRUE(SqlValueAccess::CopyBlob(value, AsBytes(blob)).has_value());
  ExpectBlob(value, blob);
  EXPECT_EQ(address, value.blob_value()->data());
  ASSERT_TRUE(SqlValueAccess::CopyText(value, Utf8View{"short"}).has_value());
  ExpectText(value, "short");
  EXPECT_EQ(address, AsBytes(value.text_value()->bytes()).data());
  ASSERT_TRUE(SqlValueAccess::CopyText(value, Utf8View{}).has_value());
  ExpectText(value, "");
  ASSERT_TRUE(SqlValueAccess::CopyBlob(value, {}).has_value());
  ExpectBlob(value, "");
  EXPECT_EQ(capacity, value.owned_capacity_bytes());

  SqlValueAccess::Release(value);
  ExpectNull(value);
  EXPECT_EQ(0U, value.owned_capacity_bytes());
}

TEST(SqlValue, PreparesColumnStorageWithoutPublishingPartialBytes) {
  SqlValue value;
  ASSERT_TRUE(SqlValueAccess::CopyText(value, Utf8View{std::string(256, 'x')}).has_value());
  const auto* address = AsBytes(value.text_value()->bytes()).data();
  const auto capacity = value.owned_capacity_bytes();
  auto output = SqlValueAccess::PrepareBytes(value, ByteCount{64});
  ASSERT_TRUE(output.has_value());
  ExpectNull(value);
  EXPECT_EQ(address, output->data());
  std::ranges::fill(*output, std::byte{0x5a});
  SqlValueAccess::CommitBytes(value, ByteCount{64}, SqlValueType::kBlob);
  ASSERT_TRUE(value.blob_value().has_value());
  EXPECT_EQ(64U, value.blob_value()->size());
  EXPECT_TRUE(std::ranges::all_of(*value.blob_value(),
                                  [](std::byte byte) { return byte == std::byte{0x5a}; }));
  EXPECT_EQ(capacity, value.owned_capacity_bytes());

  constexpr std::string_view text{"b\0c", 3};
  output = SqlValueAccess::PrepareBytes(value, ByteCount{text.size()});
  ASSERT_TRUE(output.has_value());
  ExpectNull(value);
  ASSERT_TRUE(CopyBytes(*output, AsBytes(text)).has_value());
  SqlValueAccess::CommitBytes(value, ByteCount{text.size()}, SqlValueType::kText);
  ExpectText(value, text);
  EXPECT_EQ(address, AsBytes(value.text_value()->bytes()).data());
  const auto rejected =
      SqlValueAccess::PrepareBytes(value, ByteCount{std::numeric_limits<std::size_t>::max()});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  ExpectNull(value);
  EXPECT_EQ(capacity, value.owned_capacity_bytes());
}

TEST(SqlValue, SharedColumnOwnersRemainImmutableAndCloneIndependently) {
  constexpr std::string_view text{"shared\0column", 13};
  for (const bool blob : {false, true}) {
    auto storage = std::make_shared<ByteBuffer>(
        ByteCount{text.size() + SqlValueAccess::kSharedColumnTerminatorBytes.value()});
    ASSERT_TRUE(CopyBytes(storage->mutable_view(), AsBytes(text)).has_value());
    std::weak_ptr<const ByteBuffer> lifetime = storage;
    const auto type = blob ? SqlValueType::kBlob : SqlValueType::kText;
    SqlValue first;
    SqlValue second;
    SqlValueAccess::ShareColumnBytes(first, storage, ByteCount{text.size()}, type);
    SqlValueAccess::ShareColumnBytes(second, storage, ByteCount{text.size()}, type);
    storage.reset();
    EXPECT_FALSE(lifetime.expired());
    const ByteView original = blob ? *first.blob_value() : AsBytes(first.text_value()->bytes());
    const ByteView shared = blob ? *second.blob_value() : AsBytes(second.text_value()->bytes());
    EXPECT_EQ(original.data(), shared.data());
    EXPECT_EQ(text, AsStringView(original));
    EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(first));
    SqlValue clone = first.Clone();
    SqlValue copied;
    ASSERT_TRUE(SqlValueAccess::Copy(copied, first).has_value());
    const ByteView cloned = blob ? *clone.blob_value() : AsBytes(clone.text_value()->bytes());
    const ByteView copy = blob ? *copied.blob_value() : AsBytes(copied.text_value()->bytes());
    EXPECT_EQ(text, AsStringView(cloned));
    EXPECT_EQ(text, AsStringView(copy));
    EXPECT_NE(original.data(), cloned.data());
    EXPECT_NE(original.data(), copy.data());
    SqlValue moved{std::move(first)};
    ExpectNull(first);
    SqlValueAccess::SetInteger(moved, 7);
    EXPECT_FALSE(lifetime.expired());
    EXPECT_EQ(text, AsStringView(shared));
    SqlValueAccess::Release(second);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(text, AsStringView(cloned));
    EXPECT_EQ(text, AsStringView(copy));
  }
}

TEST(SqlValue, ScalarStoresReleaseExternalOwnedBytes) {
  SqlValue value = TextValue(std::string(256, 't'));
  SqlValueAccess::SetInteger(value, 3);
  ExpectInteger(value, 3);
  EXPECT_EQ(0U, value.owned_capacity_bytes());
  value = BlobValue(std::string(256, 'b'));
  SqlValueAccess::SetNull(value);
  ExpectNull(value);
  EXPECT_EQ(0U, value.owned_capacity_bytes());
}

TEST(SqlValue, RepeatsScalarTransitionsWithoutAcquiringByteOwnership) {
  SqlValue value;
  for (const std::int64_t integer : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0},
                                     std::numeric_limits<std::int64_t>::max()}) {
    SqlValueAccess::SetInteger(value, integer);
    ExpectInteger(value, integer);
    EXPECT_EQ(0U, value.owned_capacity_bytes());
    SqlValueAccess::SetReal(value, -0.0);
    ExpectReal(value, -0.0);
    ASSERT_TRUE(value.real_value().has_value());
    EXPECT_TRUE(std::signbit(*value.real_value()));
    EXPECT_EQ(0U, value.owned_capacity_bytes());
    SqlValueAccess::SetReal(value, std::numeric_limits<double>::quiet_NaN());
    ExpectNull(value);
    SqlValueAccess::SetNull(value);
    ExpectNull(value);
    EXPECT_EQ(0U, value.owned_capacity_bytes());
  }
  const SqlValue owner = TextValue("immutable borrowed bytes");
  SqlValueAccess::BorrowStable(value, owner);
  SqlValueAccess::SetNull(value);
  ExpectNull(value);
  EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(value));
  EXPECT_EQ(0U, value.owned_capacity_bytes());
  ExpectText(owner, "immutable borrowed bytes");
}

TEST(SqlValue, PrivateCopiesPreserveAliasedExternalAndInternalBytes) {
  for (const std::string_view bytes :
       {"inline", "heap-owned bytes long enough not to use small-string storage"}) {
    SqlValue value = TextValue(bytes);
    ASSERT_TRUE(SqlValueAccess::CopyText(value, *value.text_value()).has_value());
    ExpectText(value, bytes);
    const ByteView suffix = AsBytes(value.text_value()->bytes()).subspan(1);
    ASSERT_TRUE(SqlValueAccess::CopyBlob(value, suffix).has_value());
    ExpectBlob(value, bytes.substr(1));
  }
}

TEST(SqlValue, ReusableStorageMovesAndPublicClonesOwnIndependentLogicalBytes) {
  constexpr std::string_view bytes{"a\0b", 3};
  SqlValue source;
  ASSERT_TRUE(SqlValueAccess::CopyText(source, Utf8View{bytes}).has_value());
  const auto* address = source.text_value()->data();
  const std::size_t capacity = source.owned_capacity_bytes();
  SqlValue clone = source.Clone();
  EXPECT_NE(address, clone.text_value()->data());

  SqlValue moved{std::move(source)};
  ExpectNull(source);
  EXPECT_EQ(0U, source.owned_capacity_bytes());
  ExpectText(moved, bytes);
  EXPECT_EQ(address, moved.text_value()->data());
  EXPECT_EQ(capacity, moved.owned_capacity_bytes());
  SqlValueAccess::SetNull(moved);
  EXPECT_EQ(capacity, moved.owned_capacity_bytes());
  ExpectText(clone, bytes);
}

TEST(SqlValue, CanonicalRetentionCopiesBeforeAliasedGrowAndKeepsLogicalLength) {
  ByteBuffer storage = ByteBuffer::CopyOf(AsBytes("abcd"));
  const auto grown = SqlValueAccess::RetainBytes(storage, storage.view(), ByteCount{2});
  ASSERT_TRUE(grown.has_value());
  EXPECT_EQ("abcd", AsStringView(*grown));
  ASSERT_EQ(6U, storage.size().value());
  EXPECT_EQ(std::byte{}, storage.view()[4]);
  EXPECT_EQ(std::byte{}, storage.view()[5]);

  const auto* address = storage.view().data();
  const auto overlapped =
      SqlValueAccess::RetainBytes(storage, storage.view().subspan(1, 2), ByteCount{2});
  ASSERT_TRUE(overlapped.has_value());
  EXPECT_EQ("bc", AsStringView(*overlapped));
  EXPECT_EQ(address, storage.view().data());
  EXPECT_EQ(6U, storage.size().value());
  const auto empty = SqlValueAccess::RetainBytes(storage, {});
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(address, storage.view().data());
  EXPECT_EQ(6U, storage.size().value());
}

TEST(SqlValue, CanonicalRetentionRejectsReserveOverflowWithoutChangingStorage) {
  ByteBuffer storage = ByteBuffer::CopyOf(AsBytes("original"));
  const auto* address = storage.view().data();
  const auto rejected = SqlValueAccess::RetainBytes(
      storage, storage.view(), ByteCount{std::numeric_limits<std::size_t>::max()});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  EXPECT_EQ(address, storage.view().data());
  EXPECT_EQ("original", AsStringView(storage.view()));
}

TEST(SqlValue, StableTextBorrowsCopyAndMoveWithoutExportingBorrowedClones) {
  for (const std::string_view bytes :
       {"inline", "a stable owner long enough not to use small-string storage"}) {
    const SqlValue owner = TextValue(bytes);
    SqlValue borrowed = TextValue("previous");
    SqlValueAccess::BorrowStable(borrowed, owner);
    ExpectText(borrowed, bytes);
    EXPECT_TRUE(SqlValueAccess::IsStaticBorrow(borrowed));
    EXPECT_EQ(owner.text_value()->data(), borrowed.text_value()->data());
    EXPECT_EQ(0U, borrowed.owned_capacity_bytes());

    SqlValue copied;
    ASSERT_TRUE(SqlValueAccess::Copy(copied, borrowed).has_value());
    EXPECT_TRUE(SqlValueAccess::IsStaticBorrow(copied));
    EXPECT_EQ(owner.text_value()->data(), copied.text_value()->data());
    SqlValue moved{std::move(borrowed)};
    ExpectNull(borrowed);
    ExpectText(moved, bytes);
    EXPECT_EQ(owner.text_value()->data(), moved.text_value()->data());
    SqlValue clone = moved.Clone();
    EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(clone));
    EXPECT_NE(owner.text_value()->data(), clone.text_value()->data());
    SqlValueAccess::Release(copied);
    SqlValueAccess::Release(moved);
    ExpectText(clone, bytes);
  }
}

TEST(SqlValue, StableBlobBorrowsPreserveLogicalBytesAndUnusedInternalStorage) {
  constexpr std::string_view bytes{"\xff\0b", 3};
  const SqlValue owner = BlobValue(bytes);
  SqlValue destination;
  ASSERT_TRUE(SqlValueAccess::CopyText(destination, Utf8View{std::string(256, 'x')}).has_value());
  const auto* retained = AsBytes(destination.text_value()->bytes()).data();
  const std::size_t capacity = destination.owned_capacity_bytes();
  SqlValueAccess::BorrowStable(destination, owner);
  ExpectBlob(destination, bytes);
  EXPECT_EQ(owner.blob_value()->data(), destination.blob_value()->data());
  EXPECT_EQ(capacity, destination.owned_capacity_bytes());
  EXPECT_TRUE(SqlValueAccess::IsStaticBorrow(destination));
  SqlValue clone = destination.Clone();
  EXPECT_NE(owner.blob_value()->data(), clone.blob_value()->data());
  SqlValueAccess::SetNull(destination);
  EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(destination));
  EXPECT_EQ(capacity, destination.owned_capacity_bytes());
  ASSERT_TRUE(SqlValueAccess::CopyBlob(destination, AsBytes("after")).has_value());
  EXPECT_EQ(retained, destination.blob_value()->data());
  ExpectBlob(clone, bytes);
}

TEST(SqlValue, NonstaticCopiesOwnBytesAndSelfCopiesPreserveStorage) {
  for (const bool blob : {false, true}) {
    const std::string bytes(256, 'x');
    SqlValue source = blob ? BlobValue(bytes) : TextValue(bytes);
    SqlValue destination;
    ASSERT_TRUE(SqlValueAccess::Copy(destination, source).has_value());
    EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(destination));
    EXPECT_EQ(std::strong_ordering::equal, CompareSqlValues(source, destination));
    const ByteView original = blob ? *source.blob_value() : AsBytes(source.text_value()->bytes());
    const ByteView copy =
        blob ? *destination.blob_value() : AsBytes(destination.text_value()->bytes());
    EXPECT_NE(original.data(), copy.data());
    const std::size_t capacity = destination.owned_capacity_bytes();
    ASSERT_TRUE(SqlValueAccess::Copy(destination, destination).has_value());
    SqlValueAccess::BorrowStable(destination, destination);
    EXPECT_FALSE(SqlValueAccess::IsStaticBorrow(destination));
    EXPECT_EQ(capacity, destination.owned_capacity_bytes());
    SqlValueAccess::SetNull(source);
    EXPECT_EQ(bytes,
              blob ? AsStringView(*destination.blob_value()) : destination.text_value()->bytes());
  }
}

TEST(SqlValue, ScalarAndEmptyStableCopiesPreserveTheirStorageClasses) {
  const std::array values{SqlValue{}, SqlValue::Integer(7), SqlValue::Real(-0.0), TextValue(""),
                          BlobValue("")};
  for (const SqlValue& owner : values) {
    SqlValue borrowed;
    SqlValueAccess::BorrowStable(borrowed, owner);
    EXPECT_EQ(owner.type(), borrowed.type());
    EXPECT_EQ(std::strong_ordering::equal, CompareSqlValues(owner, borrowed));
    SqlValue copied;
    ASSERT_TRUE(SqlValueAccess::Copy(copied, borrowed).has_value());
    EXPECT_EQ(owner.type(), copied.type());
    EXPECT_EQ(std::strong_ordering::equal, CompareSqlValues(owner, copied));
    if (const auto real = copied.real_value(); real.has_value()) {
      EXPECT_TRUE(std::signbit(*real));
    }
  }
}

TEST(SqlValue, NormalizesNaNToNullAndPreservesInfinityAndSignedZero) {
  const SqlValue nan = SqlValue::Real(std::numeric_limits<double>::quiet_NaN());
  const SqlValue positive_infinity = SqlValue::Real(std::numeric_limits<double>::infinity());
  const SqlValue negative_infinity = SqlValue::Real(-std::numeric_limits<double>::infinity());
  const SqlValue negative_zero = SqlValue::Real(-0.0);

  ExpectNull(nan);
  ASSERT_TRUE(positive_infinity.real_value().has_value());
  EXPECT_TRUE(std::isinf(*positive_infinity.real_value()));
  EXPECT_FALSE(std::signbit(*positive_infinity.real_value()));
  ASSERT_TRUE(negative_infinity.real_value().has_value());
  EXPECT_TRUE(std::isinf(*negative_infinity.real_value()));
  EXPECT_TRUE(std::signbit(*negative_infinity.real_value()));
  ASSERT_TRUE(negative_zero.real_value().has_value());
  EXPECT_TRUE(std::signbit(*negative_zero.real_value()));
}

TEST(SqlAffinity, NoneAndBlobAffinityAreNoOps) {
  const SqlValue text = ApplyAffinity(TextValue("01"), TypeAffinity::kNone);
  const SqlValue blob = ApplyAffinity(BlobValue("123"), TypeAffinity::kBlob);
  const SqlValue integer = ApplyAffinity(SqlValue::Integer(7), TypeAffinity::kBlob);

  ExpectText(text, "01");
  ExpectBlob(blob, "123");
  ExpectInteger(integer, 7);
}

TEST(SqlAffinity, TextAffinityUsesSQLiteNumericRendering) {
  std::array cases{
      std::pair{SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                std::string_view{"-9223372036854775808"}},
      std::pair{SqlValue::Real(1.0), std::string_view{"1.0"}},
      std::pair{SqlValue::Real(-0.0), std::string_view{"0.0"}},
      std::pair{SqlValue::Real(1.0e20), std::string_view{"1.0e+20"}},
      std::pair{SqlValue::Real(1.0e-7), std::string_view{"1.0e-07"}},
      std::pair{SqlValue::Real(std::numeric_limits<double>::infinity()), std::string_view{"Inf"}},
      std::pair{SqlValue::Real(-std::numeric_limits<double>::infinity()), std::string_view{"-Inf"}},
  };

  for (auto& [input, expected] : cases) {
    ExpectText(ApplyAffinity(std::move(input), TypeAffinity::kText), expected);
  }
}

TEST(SqlAffinity, TextAffinityMatchesSQLiteSeventeenDigitRealRendering) {
  struct Case {
    std::uint64_t bits;
    std::string_view expected;
  };
  const std::array cases{
      Case{
          .bits = UINT64_C(0xbf40557610d02cbc),
          .expected = "-0.0004984690182267197",
      },
      Case{
          .bits = UINT64_C(0xf546144501033e6f),
          .expected = "-8.2879899888294192e+256",
      },
      Case{
          .bits = UINT64_C(0x433b08f5a323d719),
          .expected = "7609675468232473.0",
      },
      Case{
          .bits = UINT64_C(0x0000000000000001),
          .expected = "4.9406564584124654e-324",
      },
      Case{
          .bits = UINT64_C(0x7fefffffffffffff),
          .expected = "1.7976931348623157e+308",
      },
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.expected);
    ExpectText(
        ApplyAffinity(SqlValue::Real(std::bit_cast<double>(test_case.bits)), TypeAffinity::kText),
        test_case.expected);
  }
}

TEST(SqlAffinity, SQLiteRealRenderingRoundTripsFiniteBinary64Values) {
  std::uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (int iteration = 0; iteration < 4096; ++iteration) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    const std::uint64_t exponent = (state >> 52U) & UINT64_C(0x7ff);
    if (exponent == UINT64_C(0x7ff) || (state << 1U) == 0) {
      continue;
    }

    const SqlValue text =
        CastValue(SqlValue::Real(std::bit_cast<double>(state)), CastTarget::kText);
    const auto text_view = text.text_value();
    ASSERT_TRUE(text_view.has_value());
    const SqlValue parsed = CastValue(TextValue(text_view->bytes()), CastTarget::kReal);

    ASSERT_TRUE(parsed.real_value().has_value());
    EXPECT_EQ(state, std::bit_cast<std::uint64_t>(*parsed.real_value()))
        << "iteration " << iteration;
  }
}

TEST(SqlAffinity, NumericAndIntegerAffinityConvertCompleteDecimalSpellings) {
  struct Case {
    std::string_view input;
    SqlValueType type;
    std::int64_t integer;
    double real;
  };
  const std::array cases{
      Case{.input = "01", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = "1.0", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = "1e0", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{.input = " 1 ", .type = SqlValueType::kInteger, .integer = 1, .real = 0.0},
      Case{
          .input = "9223372036854775807",
          .type = SqlValueType::kInteger,
          .integer = std::numeric_limits<std::int64_t>::max(),
          .real = 0.0,
      },
      Case{
          .input = "-9223372036854775808",
          .type = SqlValueType::kInteger,
          .integer = std::numeric_limits<std::int64_t>::min(),
          .real = 0.0,
      },
      Case{
          .input = "9223372036854775808",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = 9223372036854775808.0,
      },
      Case{
          .input = "-9223372036854775809",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = -9223372036854775808.0,
      },
      Case{
          .input = "9000000000000000001.",
          .type = SqlValueType::kInteger,
          .integer = 9000000000000000000,
          .real = 0.0,
      },
      Case{
          .input = "1e309",
          .type = SqlValueType::kReal,
          .integer = 0,
          .real = std::numeric_limits<double>::infinity(),
      },
      Case{.input = "1e-400", .type = SqlValueType::kInteger, .integer = 0, .real = 0.0},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    const SqlValue numeric = ApplyAffinity(TextValue(test_case.input), TypeAffinity::kNumeric);
    const SqlValue integer = ApplyAffinity(TextValue(test_case.input), TypeAffinity::kInteger);
    for (const SqlValue* result : {&numeric, &integer}) {
      if (test_case.type == SqlValueType::kInteger) {
        ExpectInteger(*result, test_case.integer);
      } else {
        ExpectReal(*result, test_case.real);
      }
    }
  }
}

TEST(SqlAffinity, NumericAffinityLeavesIncompleteAndNonDecimalTextUnchanged) {
  for (const std::string_view input : {"1x", "", " ", "0x10", "+", "."}) {
    SCOPED_TRACE(input);
    ExpectText(ApplyAffinity(TextValue(input), TypeAffinity::kNumeric), input);
  }
  ExpectBlob(ApplyAffinity(BlobValue("123"), TypeAffinity::kNumeric), "123");
}

TEST(SqlAffinity, NumericAffinityRetainsNonNumericTextStorage) {
  SqlValue input = TextValue(std::string(1024, 'x'));
  const auto input_view = input.text_value();
  ASSERT_TRUE(input_view.has_value());
  const char* const original_data = input_view->data();

  const SqlValue result = ApplyAffinity(std::move(input), TypeAffinity::kNumeric);

  const auto result_view = result.text_value();
  ASSERT_TRUE(result_view.has_value());
  EXPECT_EQ(original_data, result_view->data());
}

TEST(SqlAffinity, RealAffinityMaterializesRealValuesOnlyForNumericInputs) {
  ExpectReal(ApplyAffinity(SqlValue::Integer(7), TypeAffinity::kReal), 7.0);
  ExpectReal(ApplyAffinity(TextValue("1"), TypeAffinity::kReal), 1.0);
  ExpectReal(ApplyAffinity(TextValue("1.25"), TypeAffinity::kReal), 1.25);
  const SqlValue negative_zero = ApplyAffinity(TextValue("-0.0"), TypeAffinity::kReal);
  ASSERT_TRUE(negative_zero.real_value().has_value());
  EXPECT_FALSE(std::signbit(*negative_zero.real_value()));
  ExpectText(ApplyAffinity(TextValue("1x"), TypeAffinity::kReal), "1x");
  ExpectBlob(ApplyAffinity(BlobValue("1"), TypeAffinity::kReal), "1");
  ExpectNull(ApplyAffinity(SqlValue{}, TypeAffinity::kReal));
}

TEST(SqlNumericType, PreservesRealSyntaxAndUsesOriginalLengthIntegerFallback) {
  struct Case {
    std::string_view bytes;
    SqlValueType type = SqlValueType::kInteger;
    std::int64_t integer = 0;
    double real = 0.0;
  };
  const std::array cases{
      Case{.bytes = std::string_view{"1\0e2", 4}, .integer = 1},
      Case{.bytes = std::string_view{"1 \0e2", 5}, .integer = 1},
      Case{.bytes = std::string_view{"9007199254740992\0x", 18},
           .type = SqlValueType::kReal,
           .real = 9007199254740992.0},
      Case{.bytes = "9007199254740993", .integer = INT64_C(9007199254740993)},
      Case{.bytes = std::string_view{"2251799813685247\0x", 18},
           .integer = INT64_C(2251799813685247)},
      Case{.bytes = std::string_view{"2251799813685248\0x", 18},
           .type = SqlValueType::kReal,
           .real = 2251799813685248.0},
      Case{.bytes = std::string_view{"-2251799813685248\0x", 19},
           .integer = -INT64_C(2251799813685248)},
      Case{.bytes = std::string_view{"-2251799813685249\0x", 19},
           .type = SqlValueType::kReal,
           .real = -2251799813685249.0},
      Case{.bytes = "9223372036854775807", .integer = std::numeric_limits<std::int64_t>::max()},
      Case{.bytes = "-9223372036854775808", .integer = std::numeric_limits<std::int64_t>::min()},
      Case{.bytes = std::string_view{"9223372036854775807\0x", 21},
           .type = SqlValueType::kReal,
           .real = 9223372036854775808.0},
      Case{.bytes = std::string_view{"-9223372036854775808\0x", 22},
           .type = SqlValueType::kReal,
           .real = -9223372036854775808.0},
      Case{.bytes = std::string_view{"1.0\0x", 5}, .type = SqlValueType::kReal, .real = 1.0},
      Case{.bytes = std::string_view{"1e0\0x", 5}, .type = SqlValueType::kReal, .real = 1.0},
      Case{.bytes = "1e-400", .type = SqlValueType::kReal},
      Case{.bytes = "1e309",
           .type = SqlValueType::kReal,
           .real = std::numeric_limits<double>::infinity()},
      Case{.bytes = "-0.0", .type = SqlValueType::kReal, .real = -0.0},
      Case{.bytes = std::string_view{"-0\0e2", 5}},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.bytes);
    const SqlValue value = ApplyNumericType(TextValue(test.bytes));
    if (test.type == SqlValueType::kInteger) {
      ExpectInteger(value, test.integer);
    } else {
      ExpectReal(value, test.real);
      if (value.real_value().has_value()) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(test.real),
                  std::bit_cast<std::uint64_t>(*value.real_value()));
      }
    }
  }
}

TEST(SqlNumericType, RetainsUnconvertedOwnershipAndDistinctConversionPolicies) {
  for (const std::string_view bytes : {"", "1x", "1e", "0x10", "+", "."}) {
    ExpectText(ApplyNumericType(TextValue(bytes)), bytes);
  }
  const std::string incomplete{"1e\0x", 4};
  ExpectText(ApplyNumericType(TextValue(incomplete)), incomplete);
  const std::string malformed{"\xff\0x", 3};
  ExpectText(ApplyNumericType(TextValue(malformed)), malformed);

  const std::string payload(256, 'x');
  SqlValue text = TextValue(payload);
  ASSERT_TRUE(text.text_value().has_value());
  const char* const text_data = text.text_value()->data();
  const SqlValue retained_text = ApplyNumericType(std::move(text));
  ExpectText(retained_text, payload);
  ASSERT_TRUE(retained_text.text_value().has_value());
  EXPECT_EQ(text_data, retained_text.text_value()->data());

  const std::string bytes{"1\0e2", 4};
  SqlValue blob = BlobValue(bytes);
  ASSERT_TRUE(blob.blob_value().has_value());
  const std::byte* const blob_data = blob.blob_value()->data();
  const SqlValue retained_blob = ApplyNumericType(std::move(blob));
  ExpectBlob(retained_blob, bytes);
  ASSERT_TRUE(retained_blob.blob_value().has_value());
  EXPECT_EQ(blob_data, retained_blob.blob_value()->data());

  ExpectInteger(ApplyNumericType(SqlValue::Integer(1)), 1);
  ExpectReal(ApplyNumericType(SqlValue::Real(1.0)), 1.0);
  ExpectNull(ApplyNumericType(SqlValue{}));
  const SqlValue zero = ApplyNumericType(SqlValue::Real(-0.0));
  ASSERT_TRUE(zero.real_value().has_value());
  EXPECT_TRUE(std::signbit(*zero.real_value()));
  ExpectReal(ApplyNumericType(SqlValue::Real(std::numeric_limits<double>::infinity())),
             std::numeric_limits<double>::infinity());

  ExpectReal(ApplyNumericType(TextValue("1.0")), 1.0);
  ExpectInteger(ApplyAffinity(TextValue("1.0"), TypeAffinity::kNumeric), 1);
  ExpectInteger(CastValue(TextValue("1.0"), CastTarget::kNumeric), 1);
  ExpectReal(CoerceNumericForArithmetic(TextValue("1.0")), 1.0);
}

TEST(SqlCast, IntegerConsumesPrefixesTruncatesAndSaturates) {
  struct Case {
    std::string_view input;
    std::int64_t expected;
  };
  const std::array cases{
      Case{.input = "123abc", .expected = 123},
      Case{.input = "-2.12e-01ABC", .expected = -2},
      Case{.input = "abc", .expected = 0},
      Case{.input = "", .expected = 0},
      Case{.input = "0x1234", .expected = 0},
      Case{
          .input = "9223372036854775808",
          .expected = std::numeric_limits<std::int64_t>::max(),
      },
      Case{
          .input = "-9223372036854775809",
          .expected = std::numeric_limits<std::int64_t>::min(),
      },
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    ExpectInteger(CastValue(TextValue(test_case.input), CastTarget::kInteger), test_case.expected);
  }

  ExpectInteger(CastValue(SqlValue::Real(3.9), CastTarget::kInteger), 3);
  ExpectInteger(CastValue(SqlValue::Real(-3.9), CastTarget::kInteger), -3);
  ExpectInteger(
      CastValue(SqlValue::Real(std::numeric_limits<double>::infinity()), CastTarget::kInteger),
      std::numeric_limits<std::int64_t>::max());
  ExpectInteger(CastValue(BlobValue("42x"), CastTarget::kInteger), 42);
  ExpectNull(CastValue(SqlValue{}, CastTarget::kInteger));
}

TEST(SqlCast, RealConsumesDecimalPrefixesAndDefaultsToZero) {
  ExpectReal(CastValue(TextValue("123.5abc"), CastTarget::kReal), 123.5);
  ExpectReal(CastValue(TextValue("abc"), CastTarget::kReal), 0.0);
  ExpectReal(CastValue(TextValue("1e309"), CastTarget::kReal),
             std::numeric_limits<double>::infinity());
  ExpectReal(CastValue(SqlValue::Integer(42), CastTarget::kReal), 42.0);
  ExpectReal(CastValue(BlobValue("-2.5x"), CastTarget::kReal), -2.5);
  ExpectNull(CastValue(SqlValue{}, CastTarget::kReal));
}

TEST(SqlCast, RealUsesSQLiteDecimalToBinaryRounding) {
  struct Case {
    std::string_view input;
    std::uint64_t expected_bits;
  };
  const std::array cases{
      Case{
          .input = "803531074237660579555.423766057955561667934E105",
          .expected_bits = UINT64_C(0x5a12fe1b2d33a348),
      },
      Case{
          .input = "4.9406564584124654e-324",
          .expected_bits = UINT64_C(0x0000000000000001),
      },
      Case{.input = "2e-324", .expected_bits = UINT64_C(0x0000000000000000)},
      Case{.input = "3e-324", .expected_bits = UINT64_C(0x0000000000000001)},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.input);
    const SqlValue result = CastValue(TextValue(test_case.input), CastTarget::kReal);
    ASSERT_TRUE(result.real_value().has_value());
    EXPECT_EQ(test_case.expected_bits, std::bit_cast<std::uint64_t>(*result.real_value()));
  }
}

TEST(SqlCast, NumericUsesPrefixParsingAndSQLiteExactIntegerRange) {
  ExpectInteger(CastValue(TextValue("123abc"), CastTarget::kNumeric), 123);
  ExpectInteger(CastValue(TextValue("1.0"), CastTarget::kNumeric), 1);
  ExpectInteger(CastValue(TextValue("9.223372036e14"), CastTarget::kNumeric), 922337203600000);
  ExpectReal(CastValue(TextValue("9.223372036e15"), CastTarget::kNumeric), 9223372036000000.0);
  ExpectInteger(CastValue(TextValue("2251799813685247.0"), CastTarget::kNumeric), 2251799813685247);
  ExpectReal(CastValue(TextValue("2251799813685248.0"), CastTarget::kNumeric), 2251799813685248.0);
  ExpectInteger(CastValue(TextValue("-2251799813685248.0"), CastTarget::kNumeric),
                -2251799813685248);
  ExpectInteger(CastValue(TextValue("abc"), CastTarget::kNumeric), 0);

  const SqlValue existing_real = CastValue(SqlValue::Real(13.0), CastTarget::kNumeric);
  ExpectReal(existing_real, 13.0);
}

TEST(SqlCast, TextAndBlobCastsUseCanonicalTextBytes) {
  ExpectText(CastValue(SqlValue::Integer(123), CastTarget::kText), "123");
  ExpectText(CastValue(SqlValue::Real(1.0), CastTarget::kText), "1.0");
  ExpectText(CastValue(SqlValue::Real(-0.0), CastTarget::kText), "0.0");
  ExpectText(CastValue(BlobValue(std::string{"a\0b", 3}), CastTarget::kText),
             std::string{"a\0b", 3});
  ExpectBlob(CastValue(TextValue(std::string{"a\0b", 3}), CastTarget::kBlob),
             std::string{"a\0b", 3});
  ExpectBlob(CastValue(SqlValue::Real(1.0), CastTarget::kBlob), "1.0");
  ExpectNull(CastValue(SqlValue{}, CastTarget::kText));
  ExpectNull(CastValue(SqlValue{}, CastTarget::kBlob));
}

TEST(SqlNumericCoercion, BitwiseIntegerConversionDoesNotRequireOwnedCopies) {
  EXPECT_EQ(CoerceIntegerForBitwise(SqlValue::Integer(7)), 7);
  EXPECT_EQ(CoerceIntegerForBitwise(SqlValue::Real(7.9)), 7);
  EXPECT_EQ(CoerceIntegerForBitwise(TextValue("-12.5x")), -12);
  EXPECT_EQ(CoerceIntegerForBitwise(BlobValue("42x")), 42);
  EXPECT_EQ(CoerceIntegerForBitwise(SqlValue{}), 0);
}

TEST(SqlNumericCoercion, TruthConversionMatchesSQLiteNumericPrefixes) {
  EXPECT_EQ(EvaluateSqlTruth(SqlValue{}), SqlTruthValue::kNull);
  EXPECT_EQ(EvaluateSqlTruth(SqlValue::Integer(0)), SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlTruth(SqlValue::Real(-0.0)), SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlTruth(TextValue("2x")), SqlTruthValue::kTrue);
  EXPECT_EQ(EvaluateSqlTruth(TextValue("abc")), SqlTruthValue::kFalse);
  EXPECT_EQ(EvaluateSqlTruth(BlobValue("-3.5tail")), SqlTruthValue::kTrue);
}

TEST(SqlValueComparison, OrdersSQLiteStorageClassesAndBinaryPayloads) {
  const SqlValue null_value;
  const SqlValue number = SqlValue::Integer(1);
  const SqlValue text = TextValue("1");
  const SqlValue blob = BlobValue("1");

  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(null_value, number));
  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(number, text));
  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(text, blob));
  EXPECT_EQ(std::strong_ordering::greater, CompareSqlValues(blob, null_value));

  EXPECT_EQ(std::strong_ordering::less, CompareSqlValues(TextValue("a"), TextValue("aa")));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(TextValue(std::string{"a\0", 2}), TextValue(std::string{"a\1", 2})));
  EXPECT_EQ(std::strong_ordering::greater,
            CompareSqlValues(BlobValue(std::string{"\xff", 1}), BlobValue(std::string{"\x7f", 1})));
}

TEST(SqlValueComparison, ComparesIntegerAndRealWithoutLosingIntegerPrecision) {
  EXPECT_EQ(std::strong_ordering::greater, CompareSqlValues(SqlValue::Integer(9007199254740993LL),
                                                            SqlValue::Real(9007199254740992.0)));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(SqlValue::Integer(std::numeric_limits<std::int64_t>::max()),
                             SqlValue::Real(9223372036854775808.0)));
  EXPECT_EQ(std::strong_ordering::equal,
            CompareSqlValues(SqlValue::Integer(std::numeric_limits<std::int64_t>::min()),
                             SqlValue::Real(-9223372036854775808.0)));
  EXPECT_EQ(std::strong_ordering::equal,
            CompareSqlValues(SqlValue::Integer(0), SqlValue::Real(-0.0)));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlValues(SqlValue::Integer(0),
                             SqlValue::Real(std::numeric_limits<double>::infinity())));
  EXPECT_EQ(std::strong_ordering::greater,
            CompareSqlValues(SqlValue::Integer(0),
                             SqlValue::Real(-std::numeric_limits<double>::infinity())));
}

TEST(SqlValueComparison, ExposesExactPrimitiveNumericOrdering) {
  EXPECT_EQ(std::strong_ordering::less, CompareSqlIntegers(1, 2));
  EXPECT_EQ(std::strong_ordering::greater, CompareSqlReals(2.0, 1.0));
  EXPECT_EQ(std::strong_ordering::greater,
            CompareSqlIntegerAndReal(9007199254740993LL, 9007199254740992.0));
  EXPECT_EQ(std::strong_ordering::less,
            CompareSqlRealAndInteger(9007199254740992.0, 9007199254740993LL));
  EXPECT_EQ(std::strong_ordering::equal, CompareSqlRealAndInteger(-0.0, 0));
}

TEST(SqlComparison, PropagatesNullForOrdinaryOperators) {
  for (const SqlComparison comparison :
       {SqlComparison::kEqual, SqlComparison::kNotEqual, SqlComparison::kLess,
        SqlComparison::kLessEqual, SqlComparison::kGreater, SqlComparison::kGreaterEqual}) {
    EXPECT_EQ(SqlTruthValue::kNull,
              EvaluateSqlComparison(SqlValue{}, SqlValue::Integer(1), comparison));
    EXPECT_EQ(SqlTruthValue::kNull,
              EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, comparison));
  }
  EXPECT_EQ(SqlTruthValue::kTrue, EvaluateSqlComparison(SqlValue::Integer(1), SqlValue::Real(1.0),
                                                        SqlComparison::kEqual));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), TextValue("1"), SqlComparison::kEqual));
}

TEST(SqlComparison, IsAndIsNotCompareNullAsAValue) {
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue{}, SqlValue{}, SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue{}, SqlValue{}, SqlComparison::kIsNot));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue{}, SqlComparison::kIsNot));
  EXPECT_EQ(SqlTruthValue::kTrue,
            EvaluateSqlComparison(SqlValue::Integer(1), SqlValue::Real(1.0), SqlComparison::kIs));
  EXPECT_EQ(SqlTruthValue::kFalse,
            EvaluateSqlComparison(SqlValue::Integer(1), TextValue("1"), SqlComparison::kIs));
}

}  // namespace
}  // namespace modern_sqlite
