#include "modern_sqlite/format/record_codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] constexpr std::byte Byte(std::uint8_t value) noexcept {
  return static_cast<std::byte>(value);
}

[[nodiscard]] ByteBuffer Bytes(std::initializer_list<std::uint8_t> values) {
  ByteBuffer result{ByteCount{values.size()}};
  std::size_t offset = 0;
  for (const std::uint8_t value : values) {
    result.mutable_view()[offset] = Byte(value);
    ++offset;
  }
  return result;
}

[[nodiscard]] std::vector<SqlValue> NullValues(std::size_t count) {
  std::vector<SqlValue> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    values.emplace_back();
  }
  return values;
}

[[nodiscard]] RecordCodecOptions LegacyOptions() noexcept {
  return RecordCodecOptions{.schema_format = RecordSchemaFormat::kOne};
}

void ExpectRecordError(const Result<RecordView>& result, ErrorCode code) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(code, result.error().code());
}

void ExpectInteger(const RecordFieldView& field, std::int64_t expected) {
  EXPECT_EQ(SqlValueType::kInteger, field.type());
  ASSERT_TRUE(field.integer_value().has_value());
  EXPECT_EQ(expected, field.integer_value().value_or(0));
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

TEST(RecordSerialType, SelectsSmallestSQLiteIntegerWidth) {
  struct Vector {
    std::int64_t value;
    std::uint64_t code;
    std::size_t payload_size;
  };
  constexpr std::array<Vector, 25> kVectors{{
      {.value = std::numeric_limits<std::int64_t>::min(), .code = 6, .payload_size = 8},
      {.value = -140737488355329LL, .code = 6, .payload_size = 8},
      {.value = -140737488355328LL, .code = 5, .payload_size = 6},
      {.value = -2147483649LL, .code = 5, .payload_size = 6},
      {.value = -2147483648LL, .code = 4, .payload_size = 4},
      {.value = -8388609LL, .code = 4, .payload_size = 4},
      {.value = -8388608LL, .code = 3, .payload_size = 3},
      {.value = -32769, .code = 3, .payload_size = 3},
      {.value = -32768, .code = 2, .payload_size = 2},
      {.value = -129, .code = 2, .payload_size = 2},
      {.value = -128, .code = 1, .payload_size = 1},
      {.value = -1, .code = 1, .payload_size = 1},
      {.value = 0, .code = 8, .payload_size = 0},
      {.value = 1, .code = 9, .payload_size = 0},
      {.value = 2, .code = 1, .payload_size = 1},
      {.value = 127, .code = 1, .payload_size = 1},
      {.value = 128, .code = 2, .payload_size = 2},
      {.value = 32767, .code = 2, .payload_size = 2},
      {.value = 32768, .code = 3, .payload_size = 3},
      {.value = 8388607, .code = 3, .payload_size = 3},
      {.value = 8388608, .code = 4, .payload_size = 4},
      {.value = 2147483647LL, .code = 4, .payload_size = 4},
      {.value = 2147483648LL, .code = 5, .payload_size = 6},
      {.value = 140737488355327LL, .code = 5, .payload_size = 6},
      {.value = 140737488355328LL, .code = 6, .payload_size = 8},
  }};

  for (const Vector& vector : kVectors) {
    const SqlValue value = SqlValue::Integer(vector.value);
    const auto serial_type = SelectRecordSerialType(value);

    ASSERT_TRUE(serial_type.has_value()) << vector.value;
    EXPECT_EQ(vector.code, serial_type->code());
    EXPECT_EQ(vector.payload_size, serial_type->payload_size().value());
    EXPECT_EQ(SqlValueType::kInteger, serial_type->value_type());
  }
}

TEST(RecordSerialType, UsesOneByteIntegersBeforeSchemaFormatFour) {
  constexpr std::array<RecordSchemaFormat, 3> kLegacyFormats{
      RecordSchemaFormat::kOne,
      RecordSchemaFormat::kTwo,
      RecordSchemaFormat::kThree,
  };
  for (const RecordSchemaFormat format : kLegacyFormats) {
    for (const std::int64_t value : {0, 1}) {
      const SqlValue integer = SqlValue::Integer(value);
      const auto serial_type =
          SelectRecordSerialType(integer, RecordCodecOptions{.schema_format = format});

      ASSERT_TRUE(serial_type.has_value());
      EXPECT_EQ(1U, serial_type->code());
      EXPECT_EQ(1U, serial_type->payload_size().value());
    }
  }
}

TEST(RecordSerialType, SelectsNullRealTextAndBlobTypes) {
  const SqlValue null;
  const SqlValue real = SqlValue::Real(3.25);
  const SqlValue text = SqlValue::Text(std::string{"a\0b", 3});
  const SqlValue blob = SqlValue::Blob(Bytes({0x00, 0xff}));
  const SqlValue empty_text = SqlValue::Text("");
  const SqlValue empty_blob = SqlValue::Blob(ByteBuffer{});

  const auto null_type = SelectRecordSerialType(null);
  const auto real_type = SelectRecordSerialType(real);
  const auto text_type = SelectRecordSerialType(text);
  const auto blob_type = SelectRecordSerialType(blob);
  const auto empty_text_type = SelectRecordSerialType(empty_text);
  const auto empty_blob_type = SelectRecordSerialType(empty_blob);

  ASSERT_TRUE(null_type.has_value());
  ASSERT_TRUE(real_type.has_value());
  ASSERT_TRUE(text_type.has_value());
  ASSERT_TRUE(blob_type.has_value());
  ASSERT_TRUE(empty_text_type.has_value());
  ASSERT_TRUE(empty_blob_type.has_value());
  EXPECT_EQ(0U, null_type->code());
  EXPECT_EQ(7U, real_type->code());
  EXPECT_EQ(19U, text_type->code());
  EXPECT_EQ(16U, blob_type->code());
  EXPECT_EQ(13U, empty_text_type->code());
  EXPECT_EQ(12U, empty_blob_type->code());
}

TEST(RecordSerialType, InterpretsEveryPersistentStorageClass) {
  struct Vector {
    std::uint64_t code;
    SqlValueType type;
    std::size_t payload_size;

    constexpr Vector(std::uint64_t vector_code, SqlValueType vector_type,
                     std::size_t vector_payload_size) noexcept
        : code(vector_code), type(vector_type), payload_size(vector_payload_size) {}
  };
  constexpr std::array<Vector, 14> kVectors{{
      {0, SqlValueType::kNull, 0},
      {1, SqlValueType::kInteger, 1},
      {2, SqlValueType::kInteger, 2},
      {3, SqlValueType::kInteger, 3},
      {4, SqlValueType::kInteger, 4},
      {5, SqlValueType::kInteger, 6},
      {6, SqlValueType::kInteger, 8},
      {7, SqlValueType::kReal, 8},
      {8, SqlValueType::kInteger, 0},
      {9, SqlValueType::kInteger, 0},
      {12, SqlValueType::kBlob, 0},
      {13, SqlValueType::kText, 0},
      {128, SqlValueType::kBlob, 58},
      {129, SqlValueType::kText, 58},
  }};

  for (const Vector& vector : kVectors) {
    const auto serial_type = InterpretRecordSerialType(vector.code);

    ASSERT_TRUE(serial_type.has_value()) << vector.code;
    EXPECT_EQ(vector.code, serial_type->code());
    EXPECT_EQ(vector.type, serial_type->value_type());
    EXPECT_EQ(vector.payload_size, serial_type->payload_size().value());
  }
}

TEST(RecordSerialType, RejectsSchemaAndReservedCodes) {
  constexpr std::array<std::uint64_t, 2> kFormatFourCodes{8, 9};
  for (const std::uint64_t code : kFormatFourCodes) {
    const auto result = InterpretRecordSerialType(code, LegacyOptions());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, result.error().code());
  }
  constexpr std::array<std::uint64_t, 2> kReservedCodes{10, 11};
  for (const std::uint64_t code : kReservedCodes) {
    const auto result = InterpretRecordSerialType(code);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, result.error().code());
  }
}

TEST(RecordCodec, EncodesCanonicalHeaderSizeTransitions) {
  std::vector<SqlValue> short_header = NullValues(126);
  std::vector<SqlValue> long_header = NullValues(127);

  const auto short_record = EncodeRecord(short_header);
  const auto long_record = EncodeRecord(long_header);

  ASSERT_TRUE(short_record.has_value());
  ASSERT_TRUE(long_record.has_value());
  ASSERT_EQ(127U, short_record->size().value());
  ASSERT_EQ(129U, long_record->size().value());
  EXPECT_EQ(Byte(0x7f), short_record->view()[0]);
  EXPECT_TRUE(std::ranges::all_of(short_record->view().subspan(1),
                                  [](std::byte value) { return value == Byte(0x00); }));
  EXPECT_EQ(Byte(0x81), long_record->view()[0]);
  EXPECT_EQ(Byte(0x01), long_record->view()[1]);
  EXPECT_TRUE(std::ranges::all_of(long_record->view().subspan(2),
                                  [](std::byte value) { return value == Byte(0x00); }));

  const auto short_view = RecordView::Parse(short_record->view());
  const auto long_view = RecordView::Parse(long_record->view());
  ASSERT_TRUE(short_view.has_value());
  ASSERT_TRUE(long_view.has_value());
  EXPECT_EQ(126U, short_view->field_count());
  EXPECT_EQ(127U, long_view->field_count());
}

TEST(RecordCodec, RejectsEmptyAndOversizedFieldSets) {
  const std::span<const SqlValue> no_values;
  std::vector<SqlValue> too_many_values = NullValues(65535);

  const auto empty = EncodeRecord(no_values);
  const auto too_many = EncodeRecord(too_many_values);

  ASSERT_FALSE(empty.has_value());
  ASSERT_FALSE(too_many.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, empty.error().code());
  EXPECT_EQ(ErrorCode::kTooLarge, too_many.error().code());
}

TEST(RecordCodec, AcceptsMaximumWithoutRowidIndexFieldCount) {
  std::vector<SqlValue> values = NullValues(65534);

  const auto encoded = EncodeRecord(values);

  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(65537U, encoded->size().value());
  const auto record = RecordView::Parse(encoded->view());
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(values.size(), record->field_count());
}

TEST(RecordCodec, ReportsTheExactEncodedSize) {
  std::vector<SqlValue> values;
  values.push_back(SqlValue::Integer(128));
  values.push_back(SqlValue::Real(3.25));
  values.push_back(SqlValue::Text(std::string(58, 'x')));
  values.push_back(SqlValue::Blob(Bytes({0x00, 0xff})));

  const auto measured = EncodedRecordSize(values);
  const auto encoded = EncodeRecord(values);

  ASSERT_TRUE(measured.has_value());
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded->size(), *measured);
}

TEST(RecordCodec, AcceptsSQLiteCompatibleOverlongVarints) {
  const ByteBuffer overlong_header = Bytes({0x80, 0x03, 0x00});
  const ByteBuffer overlong_serial_type = Bytes({0x03, 0x80, 0x00});

  const auto header_view = RecordView::Parse(overlong_header.view());
  const auto serial_view = RecordView::Parse(overlong_serial_type.view());

  ASSERT_TRUE(header_view.has_value());
  ASSERT_TRUE(serial_view.has_value());
  EXPECT_EQ(1U, header_view->field_count());
  EXPECT_EQ(1U, serial_view->field_count());
  ASSERT_TRUE(header_view->field(0).has_value());
  EXPECT_EQ(SqlValueType::kNull, header_view->field(0)->type());
  EXPECT_EQ(SqlValueType::kNull, serial_view->field(0)->type());
}

TEST(RecordCodec, RejectsMalformedRecordBoundaries) {
  const std::array<ByteBuffer, 10> malformed{
      Bytes({}),           Bytes({0x00}),       Bytes({0x01}),
      Bytes({0x80}),       Bytes({0x80, 0x01}), Bytes({0x03, 0x00}),
      Bytes({0x02, 0x80}), Bytes({0x02, 0x01}), Bytes({0x02, 0x00, 0xaa}),
      Bytes({0x02, 0x0a}),
  };

  for (const ByteBuffer& record : malformed) {
    ExpectRecordError(RecordView::Parse(record.view()), ErrorCode::kCorruption);
  }
}

TEST(RecordCodec, RejectsLegacyConstantAndReservedSerialTypes) {
  const ByteBuffer constant_zero = Bytes({0x02, 0x08});
  const ByteBuffer reserved_eleven = Bytes({0x02, 0x0b});

  ExpectRecordError(RecordView::Parse(constant_zero.view(), LegacyOptions()),
                    ErrorCode::kCorruption);
  ExpectRecordError(RecordView::Parse(reserved_eleven.view()), ErrorCode::kCorruption);
}

TEST(RecordCodec, RejectsOversizedHeadersBeforeReadingTheirContents) {
  const ByteBuffer record = Bytes({0x86, 0x80, 0x04});

  ExpectRecordError(RecordView::Parse(record.view()), ErrorCode::kCorruption);
}

TEST(RecordCodec, DecodesSignedIntegersAndRealBitPatterns) {
  const ByteBuffer encoded = Bytes({
      0x09, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x80, 0x80, 0x00, 0x80, 0x00,
      0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  });

  const auto view = RecordView::Parse(encoded.view());

  ASSERT_TRUE(view.has_value());
  ASSERT_EQ(8U, view->field_count());
  constexpr std::array<std::int64_t, 6> kIntegers{
      -128,
      -32768,
      -8388608,
      -2147483648LL,
      -140737488355328LL,
      std::numeric_limits<std::int64_t>::min(),
  };
  for (std::size_t index = 0; index < kIntegers.size(); ++index) {
    const auto field = view->field(index);
    ASSERT_TRUE(field.has_value());
    ExpectInteger(*field, kIntegers[index]);
  }
  const auto negative_zero = view->field(6);
  const auto one = view->field(7);
  ASSERT_TRUE(negative_zero.has_value());
  ASSERT_TRUE(negative_zero->real_value().has_value());
  EXPECT_TRUE(std::signbit(negative_zero->real_value().value_or(0.0)));
  ASSERT_TRUE(one.has_value());
  ExpectInteger(*one, 1);
}

TEST(RecordCodec, ConvertsStoredNaNToNull) {
  const ByteBuffer encoded = Bytes({0x02, 0x07, 0x7f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

  const auto view = RecordView::Parse(encoded.view());

  ASSERT_TRUE(view.has_value());
  const auto field = view->field(0);
  ASSERT_TRUE(field.has_value());
  EXPECT_EQ(SqlValueType::kNull, field->type());
}

TEST(RecordCodec, CursorWalksBorrowedFieldsAndOwnedDecodeClonesBytes) {
  const std::string text_bytes{"A\0\xff", 3};
  std::vector<SqlValue> values;
  values.push_back(SqlValue::Text(text_bytes));
  values.push_back(SqlValue::Blob(Bytes({0x00, 0xfe})));
  values.push_back(SqlValue::Integer(42));
  auto encoded = EncodeRecord(values);
  ASSERT_TRUE(encoded.has_value());
  const auto view = RecordView::Parse(encoded->view());
  ASSERT_TRUE(view.has_value());

  RecordCursor cursor = view->cursor();
  const auto text = cursor.Next();
  const auto blob = cursor.Next();
  const auto integer = cursor.Next();
  const auto end = cursor.Next();

  ASSERT_TRUE(text.has_value());
  ASSERT_TRUE(blob.has_value());
  ASSERT_TRUE(integer.has_value());
  EXPECT_FALSE(end.has_value());
  ASSERT_TRUE(text->text_value().has_value());
  ASSERT_TRUE(blob->blob_value().has_value());
  EXPECT_EQ(text_bytes, text->text_value().value_or(Utf8View{}).bytes());
  EXPECT_TRUE(
      std::ranges::equal(blob->blob_value().value_or(ByteView{}), Bytes({0x00, 0xfe}).view()));
  ExpectInteger(*integer, 42);

  auto decoded = DecodeRecord(encoded->view());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(3U, decoded->size());
  encoded->mutable_view().back() = Byte(0x00);
  ASSERT_TRUE((*decoded)[0].text_value().has_value());
  ASSERT_TRUE((*decoded)[1].blob_value().has_value());
  EXPECT_EQ(text_bytes, (*decoded)[0].text_value().value_or(Utf8View{}).bytes());
  EXPECT_TRUE(std::ranges::equal((*decoded)[1].blob_value().value_or(ByteView{}),
                                 Bytes({0x00, 0xfe}).view()));
  EXPECT_EQ(42, (*decoded)[2].integer_value());
}

TEST(RecordCodec, ReportsOutOfRangeFieldAccess) {
  const ByteBuffer encoded = Bytes({0x02, 0x00});
  const auto view = RecordView::Parse(encoded.view());
  ASSERT_TRUE(view.has_value());

  const auto field = view->field(1);

  ASSERT_FALSE(field.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, field.error().code());
}

TEST(RecordCodec, RejectsInvalidSchemaFormatConfiguration) {
  const RecordCodecOptions invalid{
      .schema_format = InvalidEnumValue<RecordSchemaFormat>(0),
  };
  const SqlValue integer = SqlValue::Integer(1);
  const ByteBuffer encoded = Bytes({0x02, 0x00});

  const auto selected = SelectRecordSerialType(integer, invalid);
  const auto interpreted = InterpretRecordSerialType(0, invalid);
  const auto parsed = RecordView::Parse(encoded.view(), invalid);

  ASSERT_FALSE(selected.has_value());
  ASSERT_FALSE(interpreted.has_value());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, selected.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, interpreted.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, parsed.error().code());
}

[[nodiscard]] Result<RecordView> EncodeAndParse(std::span<const SqlValue> values,
                                                ByteBuffer& storage) {
  auto encoded = EncodeRecord(values);
  if (!encoded.has_value()) {
    return std::unexpected(std::move(encoded.error()));
  }
  storage = std::move(*encoded);
  return RecordView::Parse(storage.view());
}

TEST(IndexRecordComparison, AppliesStorageClassAndExactNumericOrdering) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Integer(9007199254740993LL));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> key;
  key.push_back(SqlValue::Real(9007199254740992.0));
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };

  const auto comparison = CompareIndexRecord(*record, key, columns);

  ASSERT_TRUE(comparison.has_value());
  EXPECT_EQ(std::weak_ordering::greater, comparison->ordering);
  EXPECT_FALSE(comparison->equivalent_prefix);
}

TEST(IndexRecordComparison, UsesCollationBeforeFollowingFields) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Text("A"));
  fields.push_back(SqlValue::Integer(2));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> key;
  key.push_back(SqlValue::Text("a"));
  key.push_back(SqlValue::Integer(1));
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{NoCaseCollation()},
      IndexColumnOrder{BinaryCollation()},
  };

  const auto comparison = CompareIndexRecord(*record, key, columns);

  ASSERT_TRUE(comparison.has_value());
  EXPECT_EQ(std::weak_ordering::greater, comparison->ordering);
  EXPECT_FALSE(comparison->equivalent_prefix);
}

TEST(IndexRecordComparison, TreatsRTrimEquivalentTextAsAFieldTie) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Text("value   "));
  fields.push_back(SqlValue::Integer(1));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> key;
  key.push_back(SqlValue::Text("value"));
  key.push_back(SqlValue::Integer(2));
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{RTrimCollation()},
      IndexColumnOrder{BinaryCollation()},
  };

  const auto comparison = CompareIndexRecord(*record, key, columns);

  ASSERT_TRUE(comparison.has_value());
  EXPECT_EQ(std::weak_ordering::less, comparison->ordering);
  EXPECT_FALSE(comparison->equivalent_prefix);
}

TEST(IndexRecordComparison, AppliesDirectionAndNullPlacementIndependently) {
  std::vector<SqlValue> null_field;
  null_field.emplace_back();
  ByteBuffer null_storage;
  const auto null_record = EncodeAndParse(null_field, null_storage);
  ASSERT_TRUE(null_record.has_value());
  std::vector<SqlValue> integer_key;
  integer_key.push_back(SqlValue::Integer(1));

  const std::array<IndexColumnOrder, 1> nulls_first{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kFirst},
  };
  const std::array<IndexColumnOrder, 1> nulls_last{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kAscending,
                       IndexNullPlacement::kLast},
  };

  const auto first = CompareIndexRecord(*null_record, integer_key, nulls_first);
  const auto last = CompareIndexRecord(*null_record, integer_key, nulls_last);

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(std::weak_ordering::less, first->ordering);
  EXPECT_EQ(std::weak_ordering::greater, last->ordering);

  std::vector<SqlValue> two_field;
  two_field.push_back(SqlValue::Integer(2));
  ByteBuffer two_storage;
  const auto two_record = EncodeAndParse(two_field, two_storage);
  ASSERT_TRUE(two_record.has_value());
  const std::array<IndexColumnOrder, 1> descending{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kDescending,
                       IndexNullPlacement::kLast},
  };
  const auto reversed = CompareIndexRecord(*two_record, integer_key, descending);
  ASSERT_TRUE(reversed.has_value());
  EXPECT_EQ(std::weak_ordering::less, reversed->ordering);
}

TEST(IndexRecordComparison, ComparesBlobBytesLexicographically) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Blob(Bytes({0x00, 0xff})));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> key;
  key.push_back(SqlValue::Blob(Bytes({0x01})));
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };

  const auto comparison = CompareIndexRecord(*record, key, columns);

  ASSERT_TRUE(comparison.has_value());
  EXPECT_EQ(std::weak_ordering::less, comparison->ordering);
}

TEST(IndexRecordComparison, PreservesEquivalentPrefixPolicyAndRowidTail) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Text("key"));
  fields.push_back(SqlValue::Integer(42));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> prefix;
  prefix.push_back(SqlValue::Text("key"));
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };

  const auto equivalent =
      CompareIndexRecord(*record, prefix, columns, EqualPrefixResult::kEquivalent);
  const auto before = CompareIndexRecord(*record, prefix, columns, EqualPrefixResult::kLess);
  const auto after = CompareIndexRecord(*record, prefix, columns, EqualPrefixResult::kGreater);

  ASSERT_TRUE(equivalent.has_value());
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(std::weak_ordering::equivalent, equivalent->ordering);
  EXPECT_EQ(std::weak_ordering::less, before->ordering);
  EXPECT_EQ(std::weak_ordering::greater, after->ordering);
  EXPECT_TRUE(equivalent->equivalent_prefix);
  EXPECT_TRUE(before->equivalent_prefix);
  EXPECT_TRUE(after->equivalent_prefix);

  std::vector<SqlValue> complete_key;
  complete_key.push_back(SqlValue::Text("key"));
  complete_key.push_back(SqlValue::Integer(41));
  const auto with_rowid = CompareIndexRecord(*record, complete_key, columns);
  ASSERT_TRUE(with_rowid.has_value());
  EXPECT_EQ(std::weak_ordering::greater, with_rowid->ordering);
  EXPECT_FALSE(with_rowid->equivalent_prefix);
}

TEST(IndexRecordComparison, RejectsMissingMetadataAndShortRecords) {
  std::vector<SqlValue> one_field;
  one_field.push_back(SqlValue::Integer(1));
  ByteBuffer storage;
  const auto record = EncodeAndParse(one_field, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> two_field_key;
  two_field_key.push_back(SqlValue::Integer(1));
  two_field_key.push_back(SqlValue::Integer(2));
  const std::array<IndexColumnOrder, 1> one_column{
      IndexColumnOrder{BinaryCollation()},
  };
  const std::array<IndexColumnOrder, 2> two_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };

  const auto missing_metadata = CompareIndexRecord(*record, two_field_key, one_column);
  const auto short_record = CompareIndexRecord(*record, two_field_key, two_columns);

  ASSERT_FALSE(missing_metadata.has_value());
  ASSERT_FALSE(short_record.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_metadata.error().code());
  EXPECT_EQ(ErrorCode::kCorruption, short_record.error().code());
}

TEST(IndexRecordComparison, RejectsInvalidOrderingConfiguration) {
  std::vector<SqlValue> fields;
  fields.push_back(SqlValue::Integer(1));
  ByteBuffer storage;
  const auto record = EncodeAndParse(fields, storage);
  ASSERT_TRUE(record.has_value());
  std::vector<SqlValue> key;
  key.push_back(SqlValue::Integer(1));
  const std::array<IndexColumnOrder, 1> invalid_direction{
      IndexColumnOrder{BinaryCollation(), InvalidEnumValue<IndexSortDirection>(2),
                       IndexNullPlacement::kFirst},
  };
  const std::array<IndexColumnOrder, 1> invalid_null_placement{
      IndexColumnOrder{BinaryCollation(), IndexSortDirection::kAscending,
                       InvalidEnumValue<IndexNullPlacement>(2)},
  };

  const auto direction = CompareIndexRecord(*record, key, invalid_direction);
  const auto null_placement = CompareIndexRecord(*record, key, invalid_null_placement);
  const auto prefix = CompareIndexRecord(*record, key, std::span<const IndexColumnOrder>{},
                                         InvalidEnumValue<EqualPrefixResult>(3));

  ASSERT_FALSE(direction.has_value());
  ASSERT_FALSE(null_placement.has_value());
  ASSERT_FALSE(prefix.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, direction.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, null_placement.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, prefix.error().code());
}

}  // namespace
}  // namespace modern_sqlite
