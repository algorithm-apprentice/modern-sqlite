#ifndef MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_
#define MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_

#include <cstdint>
#include <expected>
#include <limits>

namespace modern_sqlite::session_detail {

enum class CatalogGenerationError {
  kOverflow,
};

[[nodiscard]] constexpr std::expected<std::uint64_t, CatalogGenerationError> NextCatalogGeneration(
    std::uint64_t current) noexcept {
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(CatalogGenerationError::kOverflow);
  }
  return current + 1U;
}

}  // namespace modern_sqlite::session_detail

#endif  // MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_
