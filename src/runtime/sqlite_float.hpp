#ifndef MODERN_SQLITE_RUNTIME_SQLITE_FLOAT_HPP_
#define MODERN_SQLITE_RUNTIME_SQLITE_FLOAT_HPP_

#include <string>
#include <string_view>

namespace modern_sqlite::internal {

[[nodiscard]] std::string FormatSqliteReal(double value);
[[nodiscard]] double ParseSqliteReal(std::string_view token) noexcept;

}  // namespace modern_sqlite::internal

#endif  // MODERN_SQLITE_RUNTIME_SQLITE_FLOAT_HPP_
