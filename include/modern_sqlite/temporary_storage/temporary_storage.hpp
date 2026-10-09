#ifndef MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_
#define MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
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

struct RecordSorterDescriptor {
  std::size_t field_count = 0;
  std::size_t key_field_count = 0;
  // Referenced collations must outlive every sorter created from the descriptor.
  std::vector<IndexColumnOrder> key_columns{};
  RecordCodecOptions record_options{};
};

enum class RecordSorterState : std::uint8_t {
  kWriting,
  kPositioned,
  kExhausted,
  kClosed,
};

[[nodiscard]] std::string_view RecordSorterStateName(RecordSorterState state) noexcept;

class RecordSorter final {
 public:
  RecordSorter(const RecordSorter&) = delete;
  RecordSorter& operator=(const RecordSorter&) = delete;
  RecordSorter(RecordSorter&&) noexcept;
  RecordSorter& operator=(RecordSorter&&) noexcept;
  ~RecordSorter();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] RecordSorterState state() const noexcept;
  [[nodiscard]] std::size_t record_count() const noexcept;
  [[nodiscard]] ByteCount memory_usage() const noexcept;

  [[nodiscard]] Status Insert(ByteBuffer record);
  [[nodiscard]] Status Rewind();
  // The returned view remains valid until Next(), Reset(), Close(), move
  // assignment, or destruction.
  [[nodiscard]] Result<RecordView> current_record() const;
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Result<std::weak_ordering> CompareCurrent(const RecordView& record) const;
  [[nodiscard]] Status Reset();
  void Close() noexcept;

 private:
  friend class TemporaryStorageFactory;

  struct Impl;

  explicit RecordSorter(std::unique_ptr<Impl> impl) noexcept;
  [[nodiscard]] static Result<RecordSorter> Create(const RecordSorterDescriptor& descriptor,
                                                   ByteCount memory_threshold);

  std::unique_ptr<Impl> impl_;
};

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
  [[nodiscard]] Result<RecordSorter> CreateRecordSorter(
      const RecordSorterDescriptor& descriptor) const;
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
