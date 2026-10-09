#ifndef MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_
#define MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {

class Pager;

enum class TemporaryStoreMode : std::uint8_t {
  kFile,
  kMemory,
};

struct TemporaryStorageOptions {
  TemporaryStoreMode mode = TemporaryStoreMode::kFile;
  std::optional<ByteCount> sorter_memory_threshold{};

  constexpr auto operator<=>(const TemporaryStorageOptions&) const noexcept = default;
};

[[nodiscard]] Result<void> ValidateTemporaryStorageOptions(TemporaryStorageOptions options);

// Borrows the VFS and pager. Both must outlive the factory and every temporary
// file created through it.
class TemporaryStorageFactory final {
 public:
  [[nodiscard]] static Result<TemporaryStorageFactory> Create(Vfs& vfs, const Pager& pager,
                                                              TemporaryStorageOptions options = {});

  TemporaryStorageFactory(const TemporaryStorageFactory&) = delete;
  TemporaryStorageFactory& operator=(const TemporaryStorageFactory&) = delete;
  TemporaryStorageFactory(TemporaryStorageFactory&&) noexcept = default;
  TemporaryStorageFactory& operator=(TemporaryStorageFactory&&) noexcept = default;
  ~TemporaryStorageFactory() = default;

  [[nodiscard]] const TemporaryStorageOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool attached_to(const Pager& pager) const noexcept { return pager_ == &pager; }
  [[nodiscard]] bool file_spill_enabled() const noexcept;
  [[nodiscard]] ByteCount sorter_memory_threshold() const noexcept;
  [[nodiscard]] Result<std::unique_ptr<File>> CreateTemporaryFile() const;

 private:
  TemporaryStorageFactory(Vfs& vfs, const Pager& pager, TemporaryStorageOptions options) noexcept
      : vfs_(&vfs), pager_(&pager), options_(options) {}

  Vfs* vfs_;
  const Pager* pager_;
  TemporaryStorageOptions options_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_
