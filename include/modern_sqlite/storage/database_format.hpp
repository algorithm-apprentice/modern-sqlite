#ifndef MODERN_SQLITE_STORAGE_DATABASE_FORMAT_HPP_
#define MODERN_SQLITE_STORAGE_DATABASE_FORMAT_HPP_

#include <cstdint>

#include "modern_sqlite/base/result.hpp"

namespace modern_sqlite {

enum class DatabaseSchemaFormat : std::uint8_t {
  kOne = 1,
  kTwo = 2,
  kThree = 3,
  kFour = 4,
};

enum class DatabaseTextEncoding : std::uint8_t {
  kUtf8 = 1,
  kUtf16LittleEndian = 2,
  kUtf16BigEndian = 3,
};

[[nodiscard]] Result<DatabaseSchemaFormat> NormalizeSchemaFormat(std::uint32_t raw);
[[nodiscard]] Result<DatabaseTextEncoding> NormalizeTextEncoding(std::uint32_t raw);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_DATABASE_FORMAT_HPP_
