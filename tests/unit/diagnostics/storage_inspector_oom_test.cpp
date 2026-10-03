#include <cstdlib>
#include <new>

#include "modern_sqlite/diagnostics/storage_inspector.hpp"

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

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }

int main() {
  const modern_sqlite::StorageInspectionReport report;

  fail_allocations = true;
  const auto rendered = modern_sqlite::RenderStorageInspectionJson(report);
  fail_allocations = false;
  if (rendered.has_value() || rendered.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
    return 1;
  }

  fail_allocations = true;
  const auto fatal =
      modern_sqlite::RenderStorageInspectionErrorJson(modern_sqlite::ErrorCode::kCannotOpen);
  fail_allocations = false;
  if (fatal.has_value() || fatal.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
    return 1;
  }
  return 0;
}
