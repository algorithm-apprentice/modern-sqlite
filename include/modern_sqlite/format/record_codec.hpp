#ifndef MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_
#define MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

  [[nodiscard]] SqlValueType type() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> integer_value() const noexcept;
  [[nodiscard]] std::optional<double> real_value() const noexcept;
  [[nodiscard]] std::optional<Utf8View> text_value() const noexcept;
  [[nodiscard]] std::optional<ByteView> blob_value() const noexcept;
  [[nodiscard]] SqlValue ToOwned() const;

 private:
  friend class RecordCursor;

  explicit RecordFieldView(std::int64_t value) noexcept;
  explicit RecordFieldView(double value) noexcept;
  explicit RecordFieldView(Utf8View value) noexcept;
  explicit RecordFieldView(ByteView value) noexcept;

  using Storage = std::variant<std::monostate, std::int64_t, double, Utf8View, ByteView>;
  Storage storage_;
};

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

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordView& record, std::span<const SqlValue> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

[[nodiscard]] Result<IndexKeyComparison> CompareIndexRecord(
    const RecordPrefixView& record, std::span<const SqlValue> search_key,
    std::span<const IndexColumnOrder> columns,
    EqualPrefixResult equal_prefix_result = EqualPrefixResult::kEquivalent);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_FORMAT_RECORD_CODEC_HPP_
