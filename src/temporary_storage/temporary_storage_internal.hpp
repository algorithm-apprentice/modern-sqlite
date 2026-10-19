#ifndef MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_INTERNAL_HPP_
#define MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_INTERNAL_HPP_

#include <memory>
#include <span>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"

namespace modern_sqlite {

class IndexBtreeWriter;
class Pager;

namespace temporary_storage_detail {

class RelationRootLease final {
 public:
  RelationRootLease(const RelationRootLease&) = delete;
  RelationRootLease& operator=(const RelationRootLease&) = delete;
  RelationRootLease(RelationRootLease&&) noexcept;
  RelationRootLease& operator=(RelationRootLease&&) noexcept;
  ~RelationRootLease();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Pager& pager() const noexcept;
  [[nodiscard]] IndexBtreeWriter& writer() noexcept;
  [[nodiscard]] PageNumber root_page() const noexcept;
  void MarkStatementFailed() noexcept;

 private:
  friend class RelationArenaAccess;

  struct Impl;

  explicit RelationRootLease(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class RelationArenaAccess final {
 public:
  [[nodiscard]] static Result<RelationRootLease> Acquire(StatementState& state,
                                                         std::span<const IndexColumnOrder> columns);
};

class StatementStorageAccess final {
 public:
  [[nodiscard]] static StatementState& State(StatementTemporaryStorage& storage) noexcept;
  [[nodiscard]] static bool Failed(const StatementState& state) noexcept;
  static void MarkFailed(StatementState& state) noexcept;
};

}  // namespace temporary_storage_detail
}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_INTERNAL_HPP_
