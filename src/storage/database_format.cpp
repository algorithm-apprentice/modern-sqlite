#include "modern_sqlite/storage/database_format.hpp"

#include <string>

namespace modern_sqlite {

Result<DatabaseSchemaFormat> NormalizeSchemaFormat(std::uint32_t raw) {
  switch (raw) {
    case 0:
    case 1:
      return DatabaseSchemaFormat::kOne;
    case 2:
      return DatabaseSchemaFormat::kTwo;
    case 3:
      return DatabaseSchemaFormat::kThree;
    case 4:
      return DatabaseSchemaFormat::kFour;
    default:
      return std::unexpected(
          Error::Create(ErrorCode::kNotDatabase, "database has an invalid schema format"));
  }
}

Result<DatabaseTextEncoding> NormalizeTextEncoding(std::uint32_t raw) {
  switch (raw & 3U) {
    case 0:
    case 1:
      return DatabaseTextEncoding::kUtf8;
    case 2:
      return DatabaseTextEncoding::kUtf16LittleEndian;
    case 3:
      return DatabaseTextEncoding::kUtf16BigEndian;
    default:
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "unreachable database text encoding"));
  }
}

}  // namespace modern_sqlite
