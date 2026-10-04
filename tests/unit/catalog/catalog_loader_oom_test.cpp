#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/pager/read_pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"

namespace {

bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }

int main() try {
  modern_sqlite::PosixVfs vfs;
  auto opened = modern_sqlite::ReadPager::Open(vfs, FixturePath().string());
  if (!opened.has_value() || !(*opened)->BeginRead().has_value()) {
    return 1;
  }

  modern_sqlite::CatalogLoadOptions options;
  options.schema_name = std::string(256, 's');

  fail_allocations = true;
  const auto configured = modern_sqlite::LoadCatalog(**opened, options);
  fail_allocations = false;
  if (configured.has_value() ||
      configured.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
    return 1;
  }

  fail_allocations = true;
  const auto defaults = modern_sqlite::LoadCatalog(**opened);
  fail_allocations = false;
  if (defaults.has_value() || defaults.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
    return 1;
  }

  if (!(*opened)->EndRead().has_value()) {
    return 1;
  }
  return 0;
} catch (...) {
  return 1;
}
