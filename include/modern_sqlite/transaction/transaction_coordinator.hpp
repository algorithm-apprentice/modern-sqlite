#ifndef MODERN_SQLITE_TRANSACTION_TRANSACTION_COORDINATOR_HPP_
#define MODERN_SQLITE_TRANSACTION_TRANSACTION_COORDINATOR_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

class VmExecutionContext;
class Vm;

class Pager;
enum class PagerState : std::uint8_t;

namespace transaction_detail {
class CoordinatorState;
}

enum class TransactionMode : std::uint8_t {
  kDeferred,
  kImmediate,
};

enum class TransactionState : std::uint8_t {
  kAutocommit,
  kExplicit,
  kSavepoint,
};

enum class StatementAccess : std::uint8_t {
  kRead,
  kWrite,
};

enum class StatementRollbackMode : std::uint8_t {
  kTransaction,
  kStatement,
};

struct TransactionStatementOptions {
  StatementAccess access = StatementAccess::kRead;
  StatementRollbackMode rollback = StatementRollbackMode::kStatement;
};

struct TransactionWorkCounters {
  std::uint64_t pager_read_begins = 0;
  std::uint64_t pager_write_begins = 0;
  std::uint64_t pager_savepoint_creates = 0;
  std::uint64_t pager_savepoint_releases = 0;
  std::uint64_t pager_savepoint_rollbacks = 0;
  std::uint64_t pager_commits = 0;
  std::uint64_t pager_rollbacks = 0;
  std::uint64_t btree_writer_opens = 0;
};

class TransactionWriter final {
 public:
  TransactionWriter(const TransactionWriter&) = delete;
  TransactionWriter& operator=(const TransactionWriter&) = delete;
  TransactionWriter(TransactionWriter&&) = delete;
  TransactionWriter& operator=(TransactionWriter&&) = delete;
  ~TransactionWriter() = default;

  [[nodiscard]] Status InitializeDatabase(BtreeDatabaseOptions options = {});
  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree();
  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(
      std::span<const IndexColumnOrder> columns);
  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(PageNumber root_page);
  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(PageNumber root_page,
                                                        std::span<const IndexColumnOrder> columns);

 private:
  friend class transaction_detail::CoordinatorState;
  friend class VmExecutionContext;
  friend class Vm;

  TransactionWriter(transaction_detail::CoordinatorState& state, std::uint64_t token) noexcept
      : state_(&state), token_(token) {}

  [[nodiscard]] Pager& pager() noexcept;
  [[nodiscard]] Status RandomBytes(MutableByteView output);

  transaction_detail::CoordinatorState* state_;
  std::uint64_t token_;
};

class TransactionCoordinator;

class TransactionStatement final {
 public:
  TransactionStatement(const TransactionStatement&) = delete;
  TransactionStatement& operator=(const TransactionStatement&) = delete;
  TransactionStatement(TransactionStatement&&) noexcept;
  TransactionStatement& operator=(TransactionStatement&&) noexcept;
  ~TransactionStatement();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] StatementAccess access() const noexcept;
  [[nodiscard]] TransactionWriter* writer() noexcept;

  [[nodiscard]] Status Succeed();
  [[nodiscard]] Status Rollback();

 private:
  friend class TransactionCoordinator;

  struct Impl;

  explicit TransactionStatement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class TransactionCoordinator final {
 public:
  [[nodiscard]] static Result<TransactionCoordinator> Open(std::unique_ptr<Pager> pager);

  TransactionCoordinator(const TransactionCoordinator&) = delete;
  TransactionCoordinator& operator=(const TransactionCoordinator&) = delete;
  TransactionCoordinator(TransactionCoordinator&&) noexcept = default;
  TransactionCoordinator& operator=(TransactionCoordinator&&) noexcept = default;
  ~TransactionCoordinator() = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool autocommit() const noexcept;
  [[nodiscard]] TransactionState state() const noexcept;
  [[nodiscard]] bool statement_active() const noexcept;
  [[nodiscard]] std::size_t savepoint_count() const noexcept;
  [[nodiscard]] std::optional<PagerState> pager_state() const noexcept;
  [[nodiscard]] TransactionWorkCounters work_counters() const noexcept;

  [[nodiscard]] Status Begin(TransactionMode mode = TransactionMode::kDeferred);
  [[nodiscard]] Status Commit();
  [[nodiscard]] Status Rollback();
  [[nodiscard]] Status Savepoint(Utf8View name);
  [[nodiscard]] Status Release(Utf8View name);
  [[nodiscard]] Status RollbackTo(Utf8View name);

  [[nodiscard]] Result<TransactionStatement> BeginStatement(
      TransactionStatementOptions options = {});

 private:
  explicit TransactionCoordinator(
      std::shared_ptr<transaction_detail::CoordinatorState> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<transaction_detail::CoordinatorState> state_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TRANSACTION_TRANSACTION_COORDINATOR_HPP_
