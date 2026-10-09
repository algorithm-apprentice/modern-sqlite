#include "modern_sqlite/temporary_storage/temporary_storage.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/pager/pager.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kMinimumPmaPageCount = 250;
constexpr std::size_t kMaximumPmaSize = 1U << 29U;

[[nodiscard]] Error MakeError(ErrorCode code, std::string_view message) noexcept {
  try {
    return Error::Create(code, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error Misuse(std::string_view message) noexcept {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] bool IsValid(TemporaryStoreMode mode) noexcept {
  switch (mode) {
    case TemporaryStoreMode::kFile:
    case TemporaryStoreMode::kMemory:
      return true;
  }
  return false;
}

}  // namespace

Result<void> ValidateTemporaryStorageOptions(TemporaryStorageOptions options) {
  if (!IsValid(options.mode)) {
    return std::unexpected(Misuse("temporary storage mode is invalid"));
  }
  if (options.sorter_memory_threshold.has_value() &&
      (options.sorter_memory_threshold->value() == 0 ||
       options.sorter_memory_threshold->value() > kMaximumPmaSize)) {
    return std::unexpected(Misuse("sorter memory threshold must be between 1 byte and 512 MiB"));
  }
  return {};
}

Result<TemporaryStorageFactory> TemporaryStorageFactory::Create(Vfs& vfs, const Pager& pager,
                                                                TemporaryStorageOptions options) {
  auto validated = ValidateTemporaryStorageOptions(options);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  if (!pager.uses_vfs(vfs)) {
    return std::unexpected(Misuse("temporary storage VFS does not match the attached pager"));
  }
  return TemporaryStorageFactory{vfs, pager, options};
}

bool TemporaryStorageFactory::file_spill_enabled() const noexcept {
  return options_.mode == TemporaryStoreMode::kFile;
}

ByteCount TemporaryStorageFactory::sorter_memory_threshold() const noexcept {
  if (options_.sorter_memory_threshold.has_value()) {
    return *options_.sorter_memory_threshold;
  }

  const std::size_t page_size = pager_->page_size().value();
  const std::size_t minimum = page_size * kMinimumPmaPageCount;
  const std::size_t cache_pages = pager_->cache_capacity_pages();
  const std::size_t cache_size =
      cache_pages > kMaximumPmaSize / page_size ? kMaximumPmaSize : cache_pages * page_size;
  return ByteCount{std::max(minimum, std::min(cache_size, kMaximumPmaSize))};
}

Result<std::unique_ptr<File>> TemporaryStorageFactory::CreateTemporaryFile() const {
  if (!file_spill_enabled()) {
    return std::unexpected(Misuse("temporary file spill is disabled in memory mode"));
  }
  try {
    auto opened = vfs_->Open(std::nullopt, FileOpenOptions{
                                               .kind = FileKind::kTemporaryJournal,
                                               .access = FileAccessMode::kReadWrite,
                                               .create = true,
                                               .exclusive_create = true,
                                               .delete_on_close = true,
                                           });
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    return std::move(opened->file);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace modern_sqlite
