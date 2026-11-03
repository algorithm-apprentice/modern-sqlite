#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <string>

#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/storage/btree/database.hpp"
#include "tests/support/compiler_allocation_fault.hpp"

// Keep allocation-hook diagnostics independent of the allocator under test.
// NOLINTBEGIN(modernize-use-std-print)

namespace {

namespace allocation_fault = modern_sqlite::test_support::compiler_allocation_fault;

bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  return allocation_fault::Allocate(size);
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  return allocation_fault::AllocateAligned(size, alignment);
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "catalog_loader" / "sqlite-3.54.0-catalog.db";
}

[[nodiscard]] bool HasOwnedDefaults(const modern_sqlite::CatalogSnapshot& catalog) {
  const auto table = catalog.FindTable("unary_defaults");
  if (!table.has_value()) {
    return false;
  }
  const auto text_column = catalog.FindColumn(*table, "owned_text");
  const auto blob_column = catalog.FindColumn(*table, "owned_blob");
  if (!text_column.has_value() || !blob_column.has_value()) {
    return false;
  }
  const auto& text = catalog.column(*table, *text_column).missing_record_value;
  const auto& blob = catalog.column(*table, *blob_column).missing_record_value;
  return text != nullptr && blob != nullptr && text->text_value().has_value() &&
         text->text_value()->bytes() == "a long immutable default value" &&
         blob->blob_value().has_value() && blob->blob_value()->size() == 16U &&
         blob->blob_value()->back() == std::byte{0x0f};
}

[[nodiscard]] std::optional<std::size_t> CheckDefaultAllocation(
    modern_sqlite::NativeVfs& vfs, const std::string& path, std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  auto opened = BtreeDatabase::Open(vfs, path);
  if (!opened.has_value() || !(*opened)->pager().BeginRead().has_value()) {
    std::fprintf(stderr, "catalog default OOM fixture could not be opened\n");
    return std::nullopt;
  }
  allocation_fault::allocation_index.store(0, std::memory_order_relaxed);
  allocation_fault::failing_allocation = failure.value_or(0);
  allocation_fault::inject_failure = failure.has_value();
  const auto loaded = LoadCatalog(**opened);
  allocation_fault::inject_failure = false;
  const std::size_t allocations =
      allocation_fault::allocation_index.load(std::memory_order_relaxed);
  if (failure.has_value()) {
    if (loaded.has_value() || loaded.error().code() != ErrorCode::kOutOfMemory) {
      std::fprintf(stderr, "catalog default allocation prefix %zu did not report OOM\n", *failure);
      return std::nullopt;
    }
    const auto recovered = LoadCatalog(**opened);
    if (!recovered.has_value() || !HasOwnedDefaults(**recovered)) {
      std::fprintf(stderr, "catalog default allocation prefix %zu did not recover\n", *failure);
      return std::nullopt;
    }
  } else if (!loaded.has_value() || !HasOwnedDefaults(**loaded)) {
    std::fprintf(stderr, "catalog default OOM baseline has incomplete values\n");
    return std::nullopt;
  }
  if (!(*opened)->pager().EndRead().has_value()) {
    std::fprintf(stderr, "catalog default OOM fixture retained a read owner\n");
    return std::nullopt;
  }
  return allocations;
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(allocation);
}
void operator delete[](void* allocation, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(allocation);
}
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(allocation);
}
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(allocation);
}

int main() try {
  allocation_fault::owner_name = "catalog loader";
  allocation_fault::registered_allocation_thread = true;
  allocation_fault::allocation_tracking_active.store(true, std::memory_order_relaxed);
  modern_sqlite::NativeVfs vfs;
  auto opened = modern_sqlite::BtreeDatabase::Open(vfs, FixturePath().string());
  if (!opened.has_value() || !(*opened)->pager().BeginRead().has_value()) {
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

  if (!(*opened)->pager().EndRead().has_value()) {
    return 1;
  }
  const std::string defaults_path =
      (FixturePath().parent_path() / "sqlite-3.54.0-default-extraction.db").string();
  const auto allocations = CheckDefaultAllocation(vfs, defaults_path, std::nullopt);
  if (!allocations.has_value() || *allocations == 0U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < *allocations; ++failure) {
    if (!CheckDefaultAllocation(vfs, defaults_path, failure).has_value()) {
      return 1;
    }
  }
  std::fprintf(stderr, "catalog default OOM allocations=%zu prefixes=%zu recoveries=%zu\n",
               *allocations, *allocations, *allocations);
  return allocation_fault::unregistered_allocation.load(std::memory_order_relaxed) ? 1 : 0;
} catch (const std::exception& error) {
  fail_allocations = false;
  allocation_fault::inject_failure = false;
  std::fprintf(stderr, "catalog loader OOM test failed: %s\n", error.what());
  return 1;
} catch (...) {
  fail_allocations = false;
  allocation_fault::inject_failure = false;
  std::fprintf(stderr, "catalog loader OOM test failed with a nonstandard exception\n");
  return 1;
}

// NOLINTEND(modernize-use-std-print)
