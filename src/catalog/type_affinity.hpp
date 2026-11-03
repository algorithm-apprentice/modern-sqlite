#ifndef MODERN_SQLITE_CATALOG_TYPE_AFFINITY_HPP_
#define MODERN_SQLITE_CATALOG_TYPE_AFFINITY_HPP_

#include <optional>
#include <string_view>

#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite::internal {

[[nodiscard]] std::optional<TypeAffinity> DetermineColumnAffinity(
    std::optional<std::string_view> declared_type, bool strict) noexcept;

}  // namespace modern_sqlite::internal

#endif  // MODERN_SQLITE_CATALOG_TYPE_AFFINITY_HPP_
