#include "modern_sqlite/format/record_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../runtime/sql_value_access.hpp"
#include "modern_sqlite/base/coding.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kMaximumRecordHeaderSize = 98307;
constexpr std::size_t kMaximumRecordSize = 2147483645;
constexpr std::array<std::size_t, 10> kSmallSerialTypeSizes{0, 1, 2, 3, 4, 6, 8, 8, 0, 0};

static_assert(static_cast<std::size_t>(SqlValueType::kNull) == 0);
static_assert(static_cast<std::size_t>(SqlValueType::kInteger) == 1);
static_assert(static_cast<std::size_t>(SqlValueType::kReal) == 2);
static_assert(static_cast<std::size_t>(SqlValueType::kText) == 3);
static_assert(static_cast<std::size_t>(SqlValueType::kBlob) == 4);

struct RecordMeasurements {
  std::size_t header_size;
  std::size_t total_size;
};

[[nodiscard]] bool IsValidSchemaFormat(RecordSchemaFormat format) noexcept {
  switch (format) {
    case RecordSchemaFormat::kOne:
    case RecordSchemaFormat::kTwo:
    case RecordSchemaFormat::kThree:
    case RecordSchemaFormat::kFour:
      return true;
  }
  return false;
}

[[nodiscard]] bool SupportsIntegerConstants(RecordSchemaFormat format) noexcept {
  return format == RecordSchemaFormat::kFour;
}

[[nodiscard]] Error MakeError(ErrorCode code, std::string_view message) noexcept {
  try {
    return Error::Create(code, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error Misuse(std::string_view message) noexcept {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] Error Corruption(std::string_view message) noexcept {
  return MakeError(ErrorCode::kCorruption, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Result<void> ValidateOptions(RecordCodecOptions options) {
  if (!IsValidSchemaFormat(options.schema_format)) {
    return std::unexpected(Misuse("invalid record schema format"));
  }
  return {};
}

[[nodiscard]] Result<std::size_t> CheckedSizeSum(std::size_t left, std::size_t right,
                                                 std::string_view description) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return std::unexpected(TooLarge(description));
  }
  const std::size_t sum = left + right;
  if (sum > kMaximumRecordSize) {
    return std::unexpected(TooLarge(description));
  }
  return sum;
}

template <typename Value>
[[nodiscard]] Value Required(std::optional<Value> value) noexcept {
  if (!value.has_value()) {
    std::terminate();
  }
  return *value;
}

[[nodiscard]] Result<RecordMeasurements> MeasureRecord(std::span<const SqlValue> values,
                                                       RecordCodecOptions options) {
  auto valid_options = ValidateOptions(options);
  if (!valid_options.has_value()) {
    return std::unexpected(std::move(valid_options.error()));
  }
  if (values.empty()) {
    return std::unexpected(Misuse("record requires at least one field"));
  }
  if (values.size() > kMaximumRecordFieldCount) {
    return std::unexpected(TooLarge("record field count exceeds SQLite's hard limit"));
  }

  std::size_t serial_type_bytes = 0;
  std::size_t body_size = 0;
  for (const SqlValue& value : values) {
    auto serial_type = SelectRecordSerialType(value, options);
    if (!serial_type.has_value()) {
      return std::unexpected(std::move(serial_type.error()));
    }
    auto next_header =
        CheckedSizeSum(serial_type_bytes, SqliteVarintLength(serial_type->code()).value(),
                       "record header size exceeds SQLite's limit");
    if (!next_header.has_value()) {
      return std::unexpected(std::move(next_header.error()));
    }
    serial_type_bytes = *next_header;

    auto next_body = CheckedSizeSum(body_size, serial_type->payload_size().value(),
                                    "record body size exceeds SQLite's limit");
    if (!next_body.has_value()) {
      return std::unexpected(std::move(next_body.error()));
    }
    body_size = *next_body;
  }

  std::size_t header_size = serial_type_bytes + 1U;
  while (true) {
    const std::size_t size_varint_bytes = SqliteVarintLength(header_size).value();
    auto next_header = CheckedSizeSum(serial_type_bytes, size_varint_bytes,
                                      "record header size exceeds SQLite's limit");
    if (!next_header.has_value()) {
      return std::unexpected(std::move(next_header.error()));
    }
    if (*next_header == header_size) {
      break;
    }
    header_size = *next_header;
  }
  if (header_size > kMaximumRecordHeaderSize) {
    return std::unexpected(TooLarge("record header exceeds SQLite's structural limit"));
  }

  auto total_size = CheckedSizeSum(header_size, body_size, "record size exceeds SQLite's limit");
  if (!total_size.has_value()) {
    return std::unexpected(std::move(total_size.error()));
  }
  return RecordMeasurements{
      .header_size = header_size,
      .total_size = *total_size,
  };
}

struct RecordHeader {
  std::size_t initial_offset;
  std::size_t header_size;
  std::size_t field_count;
  std::size_t prefix_size;
};

[[nodiscard]] Result<RecordHeader> ReadRecordHeaderBounds(ByteView initial,
                                                          std::size_t complete_size,
                                                          RecordCodecOptions options) {
  auto valid_options = ValidateOptions(options);
  if (!valid_options.has_value()) {
    return std::unexpected(std::move(valid_options.error()));
  }
  if (initial.empty() || complete_size == 0) {
    return std::unexpected(Corruption("record header is missing"));
  }
  if (complete_size > kMaximumRecordSize) {
    return std::unexpected(Corruption("record exceeds SQLite's size limit"));
  }
  if (initial.size() > complete_size) {
    return std::unexpected(Corruption("record input exceeds the declared complete size"));
  }
  const auto decoded = DecodeSqliteVarint(initial);
  if (!decoded.has_value()) {
    return std::unexpected(Corruption("record header size varint is truncated"));
  }
  if (decoded->value > kMaximumRecordHeaderSize || decoded->value > complete_size) {
    return std::unexpected(Corruption("record header size is outside the input"));
  }
  const auto header_size = static_cast<std::size_t>(decoded->value);
  const std::size_t initial_offset = decoded->bytes_consumed.value();
  if (header_size <= initial_offset) {
    return std::unexpected(Corruption("record header contains no fields"));
  }
  return RecordHeader{
      .initial_offset = initial_offset,
      .header_size = header_size,
      .field_count = 0,
      .prefix_size = header_size,
  };
}

[[nodiscard]] Result<RecordHeader> ValidateRecordHeader(ByteView encoded, ByteCount complete_size,
                                                        std::size_t requested_fields,
                                                        RecordCodecOptions options) {
  auto header = ReadRecordHeaderBounds(encoded, complete_size.value(), options);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  if (encoded.size() < header->header_size) {
    return std::unexpected(Corruption("record header is truncated"));
  }
  std::size_t header_offset = header->initial_offset;
  std::size_t body_offset = header->header_size;
  while (header_offset < header->header_size) {
    const auto decoded_serial_type =
        DecodeSqliteVarint(encoded.subspan(header_offset, header->header_size - header_offset));
    if (!decoded_serial_type.has_value()) {
      return std::unexpected(Corruption("record serial type varint is truncated"));
    }
    auto serial_type = InterpretRecordSerialType(decoded_serial_type->value, options);
    if (!serial_type.has_value()) {
      return std::unexpected(std::move(serial_type.error()));
    }
    header_offset += decoded_serial_type->bytes_consumed.value();
    if (serial_type->payload_size().value() > complete_size.value() - body_offset) {
      return std::unexpected(Corruption("record field payload is truncated"));
    }
    body_offset += serial_type->payload_size().value();
    ++header->field_count;
    if (header->field_count > kMaximumRecordFieldCount) {
      return std::unexpected(Corruption("record field count exceeds SQLite's hard limit"));
    }
    if (header->field_count <= requested_fields) {
      header->prefix_size = body_offset;
    }
  }
  if (body_offset != complete_size.value()) {
    return std::unexpected(Corruption("record has trailing payload bytes"));
  }
  return header;
}

void WriteSignedInteger(MutableByteView output, std::int64_t value) noexcept {
  assert(!output.empty());
  assert(output.size() <= sizeof(value));
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < output.size(); ++index) {
    const std::size_t shift = (output.size() - index - 1U) * 8U;
    output[index] = static_cast<std::byte>((bits >> shift) & 0xffU);
  }
}

[[nodiscard]] std::int64_t ReadSignedInteger(ByteView input) noexcept {
  assert(!input.empty());
  assert(input.size() <= sizeof(std::int64_t));
  std::uint64_t bits = 0;
  for (const std::byte byte : input) {
    bits = (bits << 8U) | std::to_integer<std::uint8_t>(byte);
  }
  if (input.size() < sizeof(bits) && (std::to_integer<std::uint8_t>(input.front()) & 0x80U) != 0U) {
    bits |= std::numeric_limits<std::uint64_t>::max() << (input.size() * 8U);
  }
  return std::bit_cast<std::int64_t>(bits);
}

[[nodiscard]] std::weak_ordering CompareBytes(ByteView left, ByteView right) noexcept {
  const std::size_t common_size = std::min(left.size(), right.size());
  if (common_size != 0) {
    const int result = std::memcmp(left.data(), right.data(), common_size);
    if (result < 0) {
      return std::weak_ordering::less;
    }
    if (result > 0) {
      return std::weak_ordering::greater;
    }
  }
  if (left.size() < right.size()) {
    return std::weak_ordering::less;
  }
  if (left.size() > right.size()) {
    return std::weak_ordering::greater;
  }
  return std::weak_ordering::equivalent;
}

[[nodiscard]] constexpr int StorageClassRank(SqlValueType type) noexcept {
  switch (type) {
    case SqlValueType::kNull:
      return 0;
    case SqlValueType::kInteger:
    case SqlValueType::kReal:
      return 1;
    case SqlValueType::kText:
      return 2;
    case SqlValueType::kBlob:
      return 3;
  }
  return 0;
}

[[nodiscard]] std::weak_ordering Reverse(std::weak_ordering ordering) noexcept {
  if (ordering == std::weak_ordering::less) {
    return std::weak_ordering::greater;
  }
  if (ordering == std::weak_ordering::greater) {
    return std::weak_ordering::less;
  }
  return std::weak_ordering::equivalent;
}

template <typename Left, typename Right>
[[nodiscard]] std::weak_ordering CompareField(const Left& left, const Right& right,
                                              const IndexColumnOrder& column) {
  const bool left_is_null = left.type() == SqlValueType::kNull;
  const bool right_is_null = right.type() == SqlValueType::kNull;
  if (left_is_null || right_is_null) {
    if (left_is_null && right_is_null) {
      return std::weak_ordering::equivalent;
    }
    const bool left_first = column.null_placement() == IndexNullPlacement::kFirst;
    return left_is_null == left_first ? std::weak_ordering::less : std::weak_ordering::greater;
  }

  const int left_rank = StorageClassRank(left.type());
  const int right_rank = StorageClassRank(right.type());
  std::weak_ordering ordering = std::weak_ordering::equivalent;
  if (left_rank != right_rank) {
    ordering = left_rank < right_rank ? std::weak_ordering::less : std::weak_ordering::greater;
  } else if (left_rank == 1) {
    if (left.type() == SqlValueType::kInteger && right.type() == SqlValueType::kInteger) {
      ordering =
          CompareSqlIntegers(Required(left.integer_value()), Required(right.integer_value()));
    } else if (left.type() == SqlValueType::kInteger) {
      ordering =
          CompareSqlIntegerAndReal(Required(left.integer_value()), Required(right.real_value()));
    } else if (right.type() == SqlValueType::kInteger) {
      ordering =
          CompareSqlRealAndInteger(Required(left.real_value()), Required(right.integer_value()));
    } else {
      ordering = CompareSqlReals(Required(left.real_value()), Required(right.real_value()));
    }
  } else if (left.type() == SqlValueType::kText) {
    ordering =
        column.collation().Compare(Required(left.text_value()), Required(right.text_value()));
  } else {
    ordering = CompareBytes(Required(left.blob_value()), Required(right.blob_value()));
  }

  if (column.direction() == IndexSortDirection::kDescending) {
    return Reverse(ordering);
  }
  return ordering;
}

[[nodiscard]] std::weak_ordering PrefixOrdering(EqualPrefixResult result) noexcept {
  switch (result) {
    case EqualPrefixResult::kLess:
      return std::weak_ordering::less;
    case EqualPrefixResult::kEquivalent:
      return std::weak_ordering::equivalent;
    case EqualPrefixResult::kGreater:
      return std::weak_ordering::greater;
  }
  return std::weak_ordering::equivalent;
}

[[nodiscard]] bool IsValidDirection(IndexSortDirection direction) noexcept {
  return direction == IndexSortDirection::kAscending ||
         direction == IndexSortDirection::kDescending;
}

[[nodiscard]] bool IsValidNullPlacement(IndexNullPlacement placement) noexcept {
  return placement == IndexNullPlacement::kFirst || placement == IndexNullPlacement::kLast;
}

[[nodiscard]] bool IsValidEqualPrefixResult(EqualPrefixResult result) noexcept {
  return result == EqualPrefixResult::kLess || result == EqualPrefixResult::kEquivalent ||
         result == EqualPrefixResult::kGreater;
}

[[nodiscard]] Result<void> ValidateComparisonColumns(std::span<const IndexColumnOrder> columns) {
  for (const IndexColumnOrder& column : columns) {
    if (!IsValidDirection(column.direction()) || !IsValidNullPlacement(column.null_placement())) {
      return std::unexpected(Misuse("invalid index comparison ordering metadata"));
    }
  }
  return {};
}

template <typename KeyField>
[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecordFields(
    RecordCursor cursor, std::size_t complete_fields, std::span<const KeyField> search_key,
    std::span<const IndexColumnOrder> columns, EqualPrefixResult equal_prefix_result) {
  if (!IsValidEqualPrefixResult(equal_prefix_result)) {
    return std::unexpected(Misuse("invalid equal-prefix comparison result"));
  }
  if (columns.size() < search_key.size()) {
    return std::unexpected(Misuse("index comparison metadata is shorter than the search key"));
  }
  auto valid_columns = ValidateComparisonColumns(columns.first(search_key.size()));
  if (!valid_columns.has_value()) {
    return std::unexpected(std::move(valid_columns.error()));
  }
  if (complete_fields < search_key.size()) {
    return std::unexpected(Corruption("index record has fewer fields than the search key"));
  }
  if (cursor.remaining() < search_key.size()) {
    return std::unexpected(Misuse("available record prefix is shorter than the search key"));
  }

  for (std::size_t index = 0; index < search_key.size(); ++index) {
    const auto field = cursor.Next();
    if (!field.has_value()) {
      return std::unexpected(Corruption("index record field metadata ended during comparison"));
    }
    const std::weak_ordering ordering = CompareField(*field, search_key[index], columns[index]);
    if (ordering != std::weak_ordering::equivalent) {
      return IndexKeyComparison{
          .ordering = ordering,
          .equivalent_prefix = false,
      };
    }
  }
  return IndexKeyComparison{
      .ordering = PrefixOrdering(equal_prefix_result),
      .equivalent_prefix = true,
  };
}

void EncodeMeasuredRecord(std::span<const SqlValue> values, RecordCodecOptions options,
                          const RecordMeasurements& measurements, MutableByteView output) {
  auto encoded_header_size =
      EncodeSqliteVarint(measurements.header_size, output.first(measurements.header_size));
  assert(encoded_header_size.has_value());
  std::size_t header_offset = encoded_header_size->value();
  std::size_t body_offset = measurements.header_size;

  for (const SqlValue& value : values) {
    auto serial_type = SelectRecordSerialType(value, options);
    assert(serial_type.has_value());
    auto encoded_type = EncodeSqliteVarint(
        serial_type->code(), output.subspan(header_offset, body_offset - header_offset));
    assert(encoded_type.has_value());
    header_offset += encoded_type->value();

    const std::size_t payload_size = serial_type->payload_size().value();
    const MutableByteView payload = output.subspan(body_offset, payload_size);
    switch (value.type()) {
      case SqlValueType::kNull:
        break;
      case SqlValueType::kInteger:
        if (payload_size != 0) {
          WriteSignedInteger(payload, Required(value.integer_value()));
        }
        break;
      case SqlValueType::kReal: {
        const auto bits = std::bit_cast<std::uint64_t>(Required(value.real_value()));
        StoreBigEndian<std::uint64_t>(payload.first<sizeof(std::uint64_t)>(), bits);
        break;
      }
      case SqlValueType::kText: {
        const ByteView source = AsBytes(Required(value.text_value()).bytes());
        if (!source.empty()) {
          std::memcpy(payload.data(), source.data(), source.size());
        }
        break;
      }
      case SqlValueType::kBlob: {
        const ByteView source = Required(value.blob_value());
        if (!source.empty()) {
          std::memcpy(payload.data(), source.data(), source.size());
        }
        break;
      }
    }
    body_offset += payload_size;
  }

  assert(header_offset == measurements.header_size);
  assert(body_offset == measurements.total_size);
}

}  // namespace

Result<RecordSerialType> SelectRecordSerialType(const SqlValue& value, RecordCodecOptions options) {
  auto valid_options = ValidateOptions(options);
  if (!valid_options.has_value()) {
    return std::unexpected(std::move(valid_options.error()));
  }

  switch (value.type()) {
    case SqlValueType::kNull:
      return RecordSerialType{0, ByteCount{0}, SqlValueType::kNull};
    case SqlValueType::kInteger: {
      const std::int64_t integer = Required(value.integer_value());
      if (SupportsIntegerConstants(options.schema_format) && (integer == 0 || integer == 1)) {
        return RecordSerialType{
            static_cast<std::uint64_t>(8 + integer),
            ByteCount{0},
            SqlValueType::kInteger,
        };
      }

      const auto unsigned_integer = static_cast<std::uint64_t>(integer);
      const std::uint64_t magnitude = integer < 0 ? ~unsigned_integer : unsigned_integer;
      if (magnitude <= 127U) {
        return RecordSerialType{1, ByteCount{1}, SqlValueType::kInteger};
      }
      if (magnitude <= 32767U) {
        return RecordSerialType{2, ByteCount{2}, SqlValueType::kInteger};
      }
      if (magnitude <= 8388607U) {
        return RecordSerialType{3, ByteCount{3}, SqlValueType::kInteger};
      }
      if (magnitude <= 2147483647U) {
        return RecordSerialType{4, ByteCount{4}, SqlValueType::kInteger};
      }
      if (magnitude <= 140737488355327ULL) {
        return RecordSerialType{5, ByteCount{6}, SqlValueType::kInteger};
      }
      return RecordSerialType{6, ByteCount{8}, SqlValueType::kInteger};
    }
    case SqlValueType::kReal:
      return RecordSerialType{7, ByteCount{8}, SqlValueType::kReal};
    case SqlValueType::kText: {
      const std::size_t size = Required(value.text_value()).size_bytes();
      if (size > (std::numeric_limits<std::uint64_t>::max() - 13U) / 2U ||
          size > kMaximumRecordSize) {
        return std::unexpected(TooLarge("record text payload exceeds SQLite's limit"));
      }
      return RecordSerialType{
          (static_cast<std::uint64_t>(size) * 2U) + 13U,
          ByteCount{size},
          SqlValueType::kText,
      };
    }
    case SqlValueType::kBlob: {
      const std::size_t size = Required(value.blob_value()).size();
      if (size > (std::numeric_limits<std::uint64_t>::max() - 12U) / 2U ||
          size > kMaximumRecordSize) {
        return std::unexpected(TooLarge("record blob payload exceeds SQLite's limit"));
      }
      return RecordSerialType{
          (static_cast<std::uint64_t>(size) * 2U) + 12U,
          ByteCount{size},
          SqlValueType::kBlob,
      };
    }
  }
  return std::unexpected(Misuse("unknown SQL value type"));
}

Result<RecordSerialType> InterpretRecordSerialType(std::uint64_t code, RecordCodecOptions options) {
  auto valid_options = ValidateOptions(options);
  if (!valid_options.has_value()) {
    return std::unexpected(std::move(valid_options.error()));
  }
  if ((code == 8 || code == 9) && !SupportsIntegerConstants(options.schema_format)) {
    return std::unexpected(
        Corruption("record uses schema-format-four integer constant serial type"));
  }
  if (code == 10 || code == 11) {
    return std::unexpected(Corruption("record uses reserved serial type"));
  }

  switch (code) {
    case 0:
      return RecordSerialType{code, ByteCount{0}, SqlValueType::kNull};
    case 1:
      return RecordSerialType{code, ByteCount{1}, SqlValueType::kInteger};
    case 2:
      return RecordSerialType{code, ByteCount{2}, SqlValueType::kInteger};
    case 3:
      return RecordSerialType{code, ByteCount{3}, SqlValueType::kInteger};
    case 4:
      return RecordSerialType{code, ByteCount{4}, SqlValueType::kInteger};
    case 5:
      return RecordSerialType{code, ByteCount{6}, SqlValueType::kInteger};
    case 6:
      return RecordSerialType{code, ByteCount{8}, SqlValueType::kInteger};
    case 7:
      return RecordSerialType{code, ByteCount{8}, SqlValueType::kReal};
    case 8:
    case 9:
      return RecordSerialType{code, ByteCount{0}, SqlValueType::kInteger};
    default:
      break;
  }

  const std::uint64_t payload_size = (code - 12U) / 2U;
  if (payload_size > std::numeric_limits<std::size_t>::max() || payload_size > kMaximumRecordSize) {
    return std::unexpected(Corruption("record serial type payload exceeds SQLite's limit"));
  }
  return RecordSerialType{
      code,
      ByteCount{static_cast<std::size_t>(payload_size)},
      (code & 1U) == 0U ? SqlValueType::kBlob : SqlValueType::kText,
  };
}

RecordFieldView::RecordFieldView(std::int64_t value) noexcept : storage_(value) {}

RecordFieldView::RecordFieldView(double value) noexcept : storage_(value) {}

RecordFieldView::RecordFieldView(Utf8View value) noexcept : storage_(value) {}

RecordFieldView::RecordFieldView(ByteView value) noexcept : storage_(value) {}

SqlValueType RecordFieldView::type() const noexcept {
  return static_cast<SqlValueType>(storage_.index());
}

std::optional<std::int64_t> RecordFieldView::integer_value() const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<double> RecordFieldView::real_value() const noexcept {
  if (const auto* value = std::get_if<double>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<Utf8View> RecordFieldView::text_value() const noexcept {
  if (const auto* value = std::get_if<Utf8View>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

std::optional<ByteView> RecordFieldView::blob_value() const noexcept {
  if (const auto* value = std::get_if<ByteView>(&storage_); value != nullptr) {
    return *value;
  }
  return std::nullopt;
}

SqlValue RecordFieldView::ToOwned() const {
  switch (type()) {
    case SqlValueType::kNull:
      return {};
    case SqlValueType::kInteger:
      return SqlValue::Integer(Required(integer_value()));
    case SqlValueType::kReal:
      return SqlValue::Real(Required(real_value()));
    case SqlValueType::kText:
      return SqlValue::Text(std::string{Required(text_value()).bytes()});
    case SqlValueType::kBlob:
      return SqlValue::Blob(ByteBuffer::CopyOf(Required(blob_value())));
  }
  return {};
}

Result<RecordFieldView> RecordFieldView::CopyTo(ByteBuffer& storage) const {
  switch (type()) {
    case SqlValueType::kNull:
    case SqlValueType::kInteger:
    case SqlValueType::kReal:
      return *this;
    case SqlValueType::kText:
    case SqlValueType::kBlob:
      break;
  }
  const bool text = type() == SqlValueType::kText;
  const ByteView source = text ? AsBytes(Required(text_value()).bytes()) : Required(blob_value());
  auto retained = SqlValueAccess::RetainBytes(storage, source);
  if (!retained.has_value()) {
    return std::unexpected(std::move(retained.error()));
  }
  return text ? RecordFieldView{Utf8View{AsStringView(*retained)}} : RecordFieldView{*retained};
}

std::weak_ordering CompareRecordFields(const RecordFieldView& left, const RecordFieldView& right,
                                       const Collation& collation) {
  return CompareField(left, right, IndexColumnOrder{collation});
}

std::optional<RecordFieldView> RecordCursor::Next() noexcept {
  if (remaining_fields_ == 0) {
    return std::nullopt;
  }

  const auto decoded_serial_type =
      DecodeSqliteVarint(encoded_.subspan(header_offset_, header_end_ - header_offset_));
  assert(decoded_serial_type.has_value());
  const std::uint64_t code = decoded_serial_type->value;
  header_offset_ += decoded_serial_type->bytes_consumed.value();

  const std::size_t payload_size = code <= 9 ? kSmallSerialTypeSizes[static_cast<std::size_t>(code)]
                                             : static_cast<std::size_t>((code - 12U) / 2U);
  const ByteView payload = encoded_.subspan(body_offset_, payload_size);
  body_offset_ += payload_size;
  --remaining_fields_;

  switch (code) {
    case 0:
      return RecordFieldView{};
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      return RecordFieldView{ReadSignedInteger(payload)};
    case 7: {
      const auto bits = LoadBigEndian<std::uint64_t>(payload.first<sizeof(std::uint64_t)>());
      const auto value = std::bit_cast<double>(bits);
      if (std::isnan(value)) {
        return RecordFieldView{};
      }
      return RecordFieldView{value};
    }
    case 8:
    case 9:
      return RecordFieldView{static_cast<std::int64_t>(code - 8U)};
    default:
      if ((code & 1U) != 0U) {
        return RecordFieldView{Utf8View{AsStringView(payload)}};
      }
      return RecordFieldView{payload};
  }
}

Result<RecordView> RecordView::Parse(ByteView encoded, RecordCodecOptions options) {
  auto header = ValidateRecordHeader(encoded, ByteCount{encoded.size()}, 0, options);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  return RecordView{encoded, header->initial_offset, header->header_size, header->field_count};
}

RecordCursor RecordView::cursor() const noexcept {
  return RecordCursor{encoded_, header_offset_, header_end_, header_end_, field_count_};
}

Result<RecordFieldView> RecordView::field(std::size_t index) const {
  if (index >= field_count_) {
    return std::unexpected(MakeError(ErrorCode::kOutOfRange, "record field index is out of range"));
  }

  RecordCursor record_cursor = cursor();
  for (std::size_t current = 0; current < index; ++current) {
    const auto skipped = record_cursor.Next();
    if (!skipped.has_value()) {
      return std::unexpected(Corruption("record field metadata ended early"));
    }
  }
  const auto result = record_cursor.Next();
  if (!result.has_value()) {
    return std::unexpected(Corruption("record field metadata ended early"));
  }
  return *result;
}

Result<ByteCount> RecordPrefixView::RequiredHeaderSize(ByteView initial, ByteCount complete_size,
                                                       RecordCodecOptions options) {
  auto header = ReadRecordHeaderBounds(initial, complete_size.value(), options);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  return ByteCount{header->header_size};
}

Result<ByteCount> RecordPrefixView::RequiredPrefixSize(ByteView header_bytes,
                                                       ByteCount complete_size,
                                                       std::size_t requested_fields,
                                                       RecordCodecOptions options) {
  if (requested_fields == 0) {
    return std::unexpected(Misuse("record prefix requires at least one requested field"));
  }
  auto header = ValidateRecordHeader(header_bytes, complete_size, requested_fields, options);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  return ByteCount{header->prefix_size};
}

Result<RecordPrefixView> RecordPrefixView::Parse(ByteView encoded, ByteCount complete_size,
                                                 std::size_t requested_fields,
                                                 RecordCodecOptions options) {
  if (requested_fields == 0) {
    return std::unexpected(Misuse("record prefix requires at least one requested field"));
  }
  auto header = ValidateRecordHeader(encoded, complete_size, requested_fields, options);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  if (encoded.size() < header->prefix_size) {
    return std::unexpected(Corruption("record prefix field payload is truncated"));
  }
  return RecordPrefixView{encoded.first(header->prefix_size), header->initial_offset,
                          header->header_size, std::min(requested_fields, header->field_count),
                          header->field_count};
}

RecordCursor RecordPrefixView::cursor() const noexcept {
  return RecordCursor{encoded_, header_offset_, header_end_, header_end_, available_fields_};
}

Result<ByteCount> EncodedRecordSize(std::span<const SqlValue> values, RecordCodecOptions options) {
  auto measurements = MeasureRecord(values, options);
  if (!measurements.has_value()) {
    return std::unexpected(std::move(measurements.error()));
  }
  return ByteCount{measurements->total_size};
}

Result<ByteCount> EncodeRecordInto(std::span<const SqlValue> values, MutableByteView destination,
                                   RecordCodecOptions options) {
  auto measurements = MeasureRecord(values, options);
  if (!measurements.has_value()) {
    return std::unexpected(std::move(measurements.error()));
  }
  if (destination.size() < measurements->total_size) {
    return std::unexpected(Misuse("record destination is too small"));
  }
  EncodeMeasuredRecord(values, options, *measurements, destination.first(measurements->total_size));
  return ByteCount{measurements->total_size};
}

Result<ByteBuffer> EncodeRecord(std::span<const SqlValue> values, RecordCodecOptions options) {
  auto measurements = MeasureRecord(values, options);
  if (!measurements.has_value()) {
    return std::unexpected(std::move(measurements.error()));
  }
  try {
    ByteBuffer encoded{ByteCount{measurements->total_size}};
    EncodeMeasuredRecord(values, options, *measurements, encoded.mutable_view());
    return encoded;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::vector<SqlValue>> DecodeRecord(ByteView encoded, RecordCodecOptions options) {
  auto view = RecordView::Parse(encoded, options);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }

  try {
    std::vector<SqlValue> values;
    values.reserve(view->field_count());
    RecordCursor record_cursor = view->cursor();
    while (const auto field = record_cursor.Next()) {
      values.push_back(field->ToOwned());
    }
    return values;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<IndexComparisonInfo> IndexComparisonInfo::CopyOf(std::span<const IndexColumnOrder> columns) {
  return CopyOf(
      columns, IndexKeyShape{.key_field_count = columns.size(), .all_field_count = columns.size()});
}

Result<IndexComparisonInfo> IndexComparisonInfo::CopyOf(std::span<const IndexColumnOrder> columns,
                                                        IndexKeyShape shape) {
  if (columns.empty()) {
    return std::unexpected(Misuse("index comparison metadata must not be empty"));
  }
  constexpr auto kMaximumNativeFieldCount = std::numeric_limits<std::uint16_t>::max();
  if (shape.key_field_count == 0U || shape.key_field_count >= kMaximumNativeFieldCount ||
      shape.key_field_count > shape.all_field_count || columns.size() > shape.all_field_count ||
      shape.all_field_count > kMaximumNativeFieldCount) {
    return std::unexpected(Misuse("index comparison metadata has an invalid native field shape"));
  }
  auto valid = ValidateComparisonColumns(columns);
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  try {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    auto owned = std::make_shared<IndexColumnOrder[]>(shape.all_field_count);
    std::ranges::copy(columns, owned.get());
    return IndexComparisonInfo{std::move(owned), columns.size(), shape};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

IndexComparisonInfo::IndexComparisonInfo(IndexComparisonInfo&& other) noexcept
    : columns_(std::move(other.columns_)),
      column_count_(std::exchange(other.column_count_, 0U)),
      key_field_count_(std::exchange(other.key_field_count_, std::uint16_t{0})),
      all_field_count_(std::exchange(other.all_field_count_, std::uint16_t{0})) {}

IndexComparisonInfo& IndexComparisonInfo::operator=(IndexComparisonInfo&& other) noexcept {
  if (this != &other) {
    columns_ = std::move(other.columns_);
    column_count_ = std::exchange(other.column_count_, 0U);
    key_field_count_ = std::exchange(other.key_field_count_, std::uint16_t{0});
    all_field_count_ = std::exchange(other.all_field_count_, std::uint16_t{0});
  }
  return *this;
}

Result<UnpackedIndexRecord> UnpackedIndexRecord::Decode(ByteView encoded,
                                                        const IndexComparisonInfo& info,
                                                        RecordCodecOptions options) {
  if (info.columns().empty()) {
    return std::unexpected(Misuse("index record unpacking requires comparison metadata"));
  }
  try {
    const std::size_t capacity = info.key_field_count() + 1U;
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    auto fields = std::make_unique<RecordFieldView[]>(capacity);
    auto record = RecordView::Parse(encoded, options);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto cursor = record->cursor();
    std::size_t count = 0U;
    while (count < capacity) {
      auto field = cursor.Next();
      if (!field.has_value()) {
        break;
      }
      fields[count] = *field;
      ++count;
    }
    if (count == 0U || count > info.all_field_count()) {
      return std::unexpected(Corruption("unpacked index record has an invalid field count"));
    }
    return UnpackedIndexRecord{std::move(fields), count};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

UnpackedIndexRecord::UnpackedIndexRecord(UnpackedIndexRecord&& other) noexcept
    : fields_(std::move(other.fields_)), field_count_(std::exchange(other.field_count_, 0U)) {}

UnpackedIndexRecord& UnpackedIndexRecord::operator=(UnpackedIndexRecord&& other) noexcept {
  if (this != &other) {
    fields_ = std::move(other.fields_);
    field_count_ = std::exchange(other.field_count_, 0U);
  }
  return *this;
}

namespace {

[[nodiscard]] Result<std::weak_ordering> CompareRecordPrefixFields(
    RecordCursor left_cursor, std::size_t left_field_count, RecordCursor right_cursor,
    std::size_t right_field_count, std::span<const IndexColumnOrder> columns) {
  auto valid_columns = ValidateComparisonColumns(columns);
  if (!valid_columns.has_value()) {
    return std::unexpected(std::move(valid_columns.error()));
  }
  if (left_field_count < columns.size()) {
    return std::unexpected(Corruption("left record has fewer fields than comparison prefix"));
  }
  if (right_field_count < columns.size()) {
    return std::unexpected(Corruption("right record has fewer fields than comparison prefix"));
  }

  for (const IndexColumnOrder& column : columns) {
    const std::optional<RecordFieldView> left_field = left_cursor.Next();
    const std::optional<RecordFieldView> right_field = right_cursor.Next();
    if (!left_field.has_value() || !right_field.has_value()) {
      return std::unexpected(Corruption("record field metadata ended during comparison"));
    }
    const std::weak_ordering ordering = CompareField(*left_field, *right_field, column);
    if (ordering != std::weak_ordering::equivalent) {
      return ordering;
    }
  }
  return std::weak_ordering::equivalent;
}

}  // namespace

Result<std::weak_ordering> CompareRecordPrefixes(const RecordView& left, const RecordView& right,
                                                 std::span<const IndexColumnOrder> columns) {
  return CompareRecordPrefixFields(left.cursor(), left.field_count(), right.cursor(),
                                   right.field_count(), columns);
}

Result<std::weak_ordering> CompareRecordPrefixes(const RecordPrefixView& left,
                                                 const RecordView& right,
                                                 std::span<const IndexColumnOrder> columns) {
  if (left.complete_field_count() < columns.size()) {
    return std::unexpected(Corruption("left record has fewer fields than comparison prefix"));
  }
  if (left.field_count() < columns.size()) {
    return std::unexpected(Misuse("left record prefix does not contain all comparison fields"));
  }
  return CompareRecordPrefixFields(left.cursor(), left.field_count(), right.cursor(),
                                   right.field_count(), columns);
}

Result<IndexKeyComparison> CompareIndexRecord(const RecordView& record,
                                              std::span<const SqlValue> search_key,
                                              std::span<const IndexColumnOrder> columns,
                                              EqualPrefixResult equal_prefix_result) {
  return CompareIndexRecordFields(record.cursor(), record.field_count(), search_key, columns,
                                  equal_prefix_result);
}

Result<IndexKeyComparison> CompareIndexRecord(const RecordPrefixView& record,
                                              std::span<const SqlValue> search_key,
                                              std::span<const IndexColumnOrder> columns,
                                              EqualPrefixResult equal_prefix_result) {
  return CompareIndexRecordFields(record.cursor(), record.complete_field_count(), search_key,
                                  columns, equal_prefix_result);
}

Result<IndexKeyComparison> CompareIndexRecord(const RecordView& record,
                                              std::span<const RecordFieldView> search_key,
                                              std::span<const IndexColumnOrder> columns,
                                              EqualPrefixResult equal_prefix_result) {
  return CompareIndexRecordFields(record.cursor(), record.field_count(), search_key, columns,
                                  equal_prefix_result);
}

Result<IndexKeyComparison> CompareIndexRecord(const RecordPrefixView& record,
                                              std::span<const RecordFieldView> search_key,
                                              std::span<const IndexColumnOrder> columns,
                                              EqualPrefixResult equal_prefix_result) {
  return CompareIndexRecordFields(record.cursor(), record.complete_field_count(), search_key,
                                  columns, equal_prefix_result);
}

}  // namespace modern_sqlite
