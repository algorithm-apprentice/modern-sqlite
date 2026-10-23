#ifndef MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_
#define MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

namespace record_codec_detail {
class ValidatedRecordAccess;
}  // namespace record_codec_detail

using RecordSchemaFormat = DatabaseSchemaFormat;
inline constexpr std::uint32_t kVariableSerialTypeBase = 12U;

struct RecordCodecOptions {
  RecordSchemaFormat schema_format = RecordSchemaFormat::kFour;
};

class RecordSerialType final {
 public:
  [[nodiscard]] constexpr std::uint64_t code() const noexcept { return code_; }
  [[nodiscard]] constexpr ByteCount payload_size() const noexcept { return payload_size_; }
  [[nodiscard]] constexpr SqlValueType value_type() const noexcept { return value_type_; }

 private:
  friend Result<RecordSerialType> SelectRecordSerialType(const SqlValue&, RecordCodecOptions);
  friend Result<RecordSerialType> InterpretRecordSerialType(std::uint64_t, RecordCodecOptions);

  constexpr RecordSerialType(std::uint64_t code, ByteCount payload_size,
                             SqlValueType value_type) noexcept
      : code_(code), payload_size_(payload_size), value_type_(value_type) {}

  std::uint64_t code_;
  ByteCount payload_size_;
  SqlValueType value_type_;
};

[[nodiscard]] Result<RecordSerialType> SelectRecordSerialType(const SqlValue& value,
                                                              RecordCodecOptions options = {});
[[nodiscard]] Result<RecordSerialType> InterpretRecordSerialType(std::uint64_t code,
                                                                 RecordCodecOptions options = {});

class RecordFieldView final {
 public:
  RecordFieldView() noexcept = default;

  // Consumes one native Column field, not a complete persistent record.
  [[nodiscard]] static Result<RecordFieldView> ParseColumn(std::uint32_t serial_type,
                                                           ByteView payload) noexcept;
  [[nodiscard]] SqlValueType type() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> integer_value() const noexcept;
  [[nodiscard]] std::optional<double> real_value() const noexcept;
  [[nodiscard]] std::optional<Utf8View> text_value() const noexcept;
  [[nodiscard]] std::optional<ByteView> blob_value() const noexcept;
  [[nodiscard]] SqlValue ToOwned() const;
  // Reuses caller storage; returned byte views borrow it until modification or move.
  [[nodiscard]] Result<RecordFieldView> CopyTo(ByteBuffer& storage) const;

 private:
  friend class RecordCursor;

  explicit RecordFieldView(std::int64_t value) noexcept;
  explicit RecordFieldView(double value) noexcept;
  explicit RecordFieldView(Utf8View value) noexcept;
  explicit RecordFieldView(ByteView value) noexcept;
  [[nodiscard]] static RecordFieldView DecodePayload(std::uint64_t serial_type,
                                                     ByteView payload) noexcept;

  using Storage = std::variant<std::monostate, std::int64_t, double, Utf8View, ByteView>;
  Storage storage_;
};

[[nodiscard]] std::weak_ordering CompareRecordFields(const RecordFieldView& left,
                                                     const RecordFieldView& right,
                                                     const Collation& collation);

class RecordCursor final {
 public:
  [[nodiscard]] std::optional<RecordFieldView> Next() noexcept;
  [[nodiscard]] std::size_t remaining() const noexcept { return remaining_fields_; }

 private:
  friend class RecordView;
  friend class RecordPrefixView;

  RecordCursor(ByteView encoded, std::size_t header_offset, std::size_t header_end,
               std::size_t body_offset, std::size_t field_count) noexcept
      : encoded_(encoded),
        header_offset_(header_offset),
        header_end_(header_end),
        body_offset_(body_offset),
        remaining_fields_(field_count) {}

  ByteView encoded_;
  std::size_t header_offset_;
  std::size_t header_end_;
  std::size_t body_offset_;
  std::size_t remaining_fields_;
};

// Borrows one complete immutable encoded record. The encoded bytes must
// outlive the view and every cursor or field view derived from it.
class RecordView final {
 public:
  [[nodiscard]] static Result<RecordView> Parse(ByteView encoded, RecordCodecOptions options = {});

  [[nodiscard]] ByteView encoded() const noexcept { return encoded_; }
  [[nodiscard]] ByteCount header_size() const noexcept { return ByteCount{header_end_}; }
  [[nodiscard]] std::size_t field_count() const noexcept { return field_count_; }
  [[nodiscard]] RecordCursor cursor() const noexcept;
  [[nodiscard]] Result<RecordFieldView> field(std::size_t index) const;

 private:
  friend class record_codec_detail::ValidatedRecordAccess;

  RecordView(ByteView encoded, std::size_t header_offset, std::size_t header_end,
             std::size_t field_count) noexcept
      : encoded_(encoded),
        header_offset_(header_offset),
        header_end_(header_end),
        field_count_(field_count) {}

  ByteView encoded_;
  std::size_t header_offset_;
  std::size_t header_end_;
  std::size_t field_count_;
};

// Borrows the complete header and available initial field payloads of a
// record with a known full size. The bytes must outlive all derived views.
class RecordPrefixView final {
 public:
  [[nodiscard]] static Result<ByteCount> RequiredHeaderSize(ByteView initial,
                                                            ByteCount complete_size,
                                                            RecordCodecOptions options = {});
  [[nodiscard]] static Result<ByteCount> RequiredPrefixSize(ByteView header,
                                                            ByteCount complete_size,
                                                            std::size_t requested_fields,
                                                            RecordCodecOptions options = {});
  [[nodiscard]] static Result<RecordPrefixView> Parse(ByteView encoded, ByteCount complete_size,
                                                      std::size_t requested_fields,
                                                      RecordCodecOptions options = {});

  [[nodiscard]] ByteView encoded() const noexcept { return encoded_; }
  [[nodiscard]] ByteCount header_size() const noexcept { return ByteCount{header_end_}; }
  [[nodiscard]] std::size_t field_count() const noexcept { return available_fields_; }
  [[nodiscard]] std::size_t complete_field_count() const noexcept { return complete_fields_; }
  [[nodiscard]] RecordCursor cursor() const noexcept;

 private:
  RecordPrefixView(ByteView encoded, std::size_t header_offset, std::size_t header_end,
                   std::size_t available_fields, std::size_t complete_fields) noexcept
      : encoded_(encoded),
        header_offset_(header_offset),
        header_end_(header_end),
        available_fields_(available_fields),
        complete_fields_(complete_fields) {}

  ByteView encoded_;
  std::size_t header_offset_;
  std::size_t header_end_;
  std::size_t available_fields_;
  std::size_t complete_fields_;
};

struct RecordColumnField {
  std::uint32_t serial_type;
  ByteOffset offset;
  ByteCount size;

  [[nodiscard]] SqlValueType type() const noexcept;
};

class RecordColumnLayout final {
 public:
  RecordColumnLayout() = default;
  RecordColumnLayout(const RecordColumnLayout&) = delete;
  RecordColumnLayout& operator=(const RecordColumnLayout&) = delete;
  RecordColumnLayout(RecordColumnLayout&&) noexcept = default;
  RecordColumnLayout& operator=(RecordColumnLayout&&) noexcept = default;

  [[nodiscard]] Status Prepare(std::size_t field_count) noexcept;
  void Invalidate() noexcept;
  [[nodiscard]] Status Reset(ByteView initial, ByteCount complete_size,
                             RecordCodecOptions options = {}) noexcept;
  [[nodiscard]] ByteCount header_size() const noexcept { return ByteCount{header_size_}; }
  [[nodiscard]] bool needs_header(std::size_t field) const noexcept {
    return ready_ && parsed_fields_ <= field && header_offset_ < header_size_;
  }
  [[nodiscard]] Result<std::optional<RecordColumnField>> field(std::size_t index,
                                                               ByteView header) noexcept;

 private:
  struct Entry {
    std::uint32_t serial_type = 0;
    std::uint32_t offset = 0;
  };

  std::vector<Entry> fields_;
  std::uint32_t payload_size_ = 0;
  std::uint32_t header_size_ = 0;
  std::uint32_t header_offset_ = 0;
  std::uint32_t data_offset_ = 0;
  std::uint32_t parsed_fields_ = 0;
  bool ready_ = false;
};

[[nodiscard]] Result<ByteCount> EncodedRecordSize(std::span<const SqlValue> values,
                                                  RecordCodecOptions options = {});
[[nodiscard]] Result<ByteCount> EncodeRecordInto(std::span<const SqlValue> values,
                                                 MutableByteView destination,
                                                 RecordCodecOptions options = {});
[[nodiscard]] Result<ByteBuffer> EncodeRecord(std::span<const SqlValue> values,
                                              RecordCodecOptions options = {});
[[nodiscard]] Result<std::vector<SqlValue>> DecodeRecord(ByteView encoded,
                                                         RecordCodecOptions options = {});

enum class IndexSortDirection : std::uint8_t {
  kAscending,
  kDescending,
};

enum class IndexNullPlacement : std::uint8_t {
  kFirst,
  kLast,
};

class IndexColumnOrder final {
 public:
  IndexColumnOrder() noexcept : IndexColumnOrder(BinaryCollation()) {}

  explicit IndexColumnOrder(const Collation& collation,
                            IndexSortDirection direction = IndexSortDirection::kAscending,
                            IndexNullPlacement null_placement = IndexNullPlacement::kFirst) noexcept
      : collation_(&collation), direction_(direction), null_placement_(null_placement) {}

  [[nodiscard]] const Collation& collation() const noexcept { return *collation_; }
  [[nodiscard]] IndexSortDirection direction() const noexcept { return direction_; }
  [[nodiscard]] IndexNullPlacement null_placement() const noexcept { return null_placement_; }

 private:
  // The referenced collation must outlive every comparison using this value.
  const Collation* collation_;
  IndexSortDirection direction_;
  IndexNullPlacement null_placement_;
};

// Native KeyInfo counts are independent of the supplied operational order prefix.
struct IndexKeyShape {
  std::size_t key_field_count;
  std::size_t all_field_count;
};

class IndexComparisonInfo final {
 public:
  [[nodiscard]] static Result<IndexComparisonInfo> CopyOf(
      std::span<const IndexColumnOrder> columns);
  [[nodiscard]] static Result<IndexComparisonInfo> CopyOf(std::span<const IndexColumnOrder> columns,
                                                          IndexKeyShape shape);

  IndexComparisonInfo(const IndexComparisonInfo&) noexcept = default;
  IndexComparisonInfo& operator=(const IndexComparisonInfo&) noexcept = default;
  IndexComparisonInfo(IndexComparisonInfo&& other) noexcept;
  IndexComparisonInfo& operator=(IndexComparisonInfo&& other) noexcept;
  ~IndexComparisonInfo() = default;

  [[nodiscard]] std::span<const IndexColumnOrder> columns() const noexcept {
    return column_count_ == 0U ? std::span<const IndexColumnOrder>{}
                               : std::span<const IndexColumnOrder>{columns_.get(), column_count_};
  }
  [[nodiscard]] std::span<const IndexColumnOrder> all_columns() const noexcept {
    return all_field_count_ == 0U
               ? std::span<const IndexColumnOrder>{}
               : std::span<const IndexColumnOrder>{columns_.get(), all_field_count_};
  }
  [[nodiscard]] std::size_t key_field_count() const noexcept { return key_field_count_; }
  [[nodiscard]] std::size_t all_field_count() const noexcept { return all_field_count_; }

 private:
  IndexComparisonInfo(std::shared_ptr<const IndexColumnOrder[]> columns, std::size_t column_count,
                      IndexKeyShape shape) noexcept
      : columns_(std::move(columns)),
        column_count_(column_count),
        key_field_count_(static_cast<std::uint16_t>(shape.key_field_count)),
        all_field_count_(static_cast<std::uint16_t>(shape.all_field_count)) {}

  std::shared_ptr<const IndexColumnOrder[]> columns_;
  std::size_t column_count_;
  std::uint16_t key_field_count_;
  std::uint16_t all_field_count_;
};

// Field byte views borrow the encoded input until it is changed or destroyed.
class UnpackedIndexRecord final {
 public:
  [[nodiscard]] static Result<UnpackedIndexRecord> Decode(ByteView encoded,
                                                          const IndexComparisonInfo& info,
                                                          RecordCodecOptions options = {});

  UnpackedIndexRecord(const UnpackedIndexRecord&) = delete;
  UnpackedIndexRecord& operator=(const UnpackedIndexRecord&) = delete;
  UnpackedIndexRecord(UnpackedIndexRecord&& other) noexcept;
  UnpackedIndexRecord& operator=(UnpackedIndexRecord&& other) noexcept;
  ~UnpackedIndexRecord() = default;

  [[nodiscard]] std::span<const RecordFieldView> fields() const noexcept {
    return field_count_ == 0U ? std::span<const RecordFieldView>{}
                              : std::span<const RecordFieldView>{fields_.get(), field_count_};
  }

 private:
  UnpackedIndexRecord(std::unique_ptr<RecordFieldView[]> fields, std::size_t count) noexcept
      : fields_(std::move(fields)), field_count_(count) {}

  std::unique_ptr<RecordFieldView[]> fields_;
  std::size_t field_count_;
};

enum class EqualPrefixResult : std::uint8_t {
  // Ordering of the encoded record relative to an equivalent search prefix.
  kLess,
  kEquivalent,
  kGreater,
};

struct IndexKeyComparison {
  std::weak_ordering ordering;
  bool equivalent_prefix;
};

[[nodiscard]] Result<std::weak_ordering> CompareRecordPrefixes(
    const RecordView& left, const RecordView& right, std::span<const IndexColumnOrder> columns);
[[nodiscard]] Result<std::weak_ordering> CompareRecordPrefixes(
    const RecordPrefixView& left, const RecordView& right,
    std::span<const IndexColumnOrder> columns);
[[nodiscard]] Result<std::weak_ordering> CompareRecordPrefixes(
    std::span<const SqlValue> left, std::span<const SqlValue> right,
    std::span<const IndexColumnOrder> columns);

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordView& record, std::span<const SqlValue> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordPrefixView& record, std::span<const SqlValue> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordView& record, std::span<const RecordFieldView> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordPrefixView& record, std::span<const RecordFieldView> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_
