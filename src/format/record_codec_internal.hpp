#ifndef MODERN_SQLITE_FORMAT_RECORD_CODEC_INTERNAL_HPP_
#define MODERN_SQLITE_FORMAT_RECORD_CODEC_INTERNAL_HPP_

#include <cstddef>
#include <optional>

#include "modern_sqlite/format/record_codec.hpp"

namespace modern_sqlite::record_codec_detail {

struct ValidatedRecordLayout {
  std::size_t encoded_size = 0;
  std::size_t header_offset = 0;
  std::size_t header_end = 0;
  std::size_t field_count = 0;
};

class ValidatedRecordAccess final {
 public:
  [[nodiscard]] static ValidatedRecordLayout Capture(const RecordView& record) noexcept {
    return ValidatedRecordLayout{
        .encoded_size = record.encoded_.size(),
        .header_offset = record.header_offset_,
        .header_end = record.header_end_,
        .field_count = record.field_count_,
    };
  }

  [[nodiscard]] static std::optional<RecordView> Rebind(ByteView encoded,
                                                        ValidatedRecordLayout layout) noexcept {
    if (encoded.size() != layout.encoded_size || layout.field_count == 0U ||
        layout.header_offset >= layout.header_end || layout.header_end > encoded.size()) {
      return std::nullopt;
    }
    return RecordView{encoded, layout.header_offset, layout.header_end, layout.field_count};
  }
};

}  // namespace modern_sqlite::record_codec_detail

#endif  // MODERN_SQLITE_FORMAT_RECORD_CODEC_INTERNAL_HPP_
