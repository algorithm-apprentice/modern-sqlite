#ifndef MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class Pager;
class TableBtreeWriter;
class TableBtreeMutationCursor;
class IndexBtreeWriter;

namespace transaction_detail {
class CoordinatorState;
}

namespace btree_internal {
class BtreeWriterCore;
}

enum class BtreeInsertMode : std::uint8_t {
  kInsertOnly,
  kReplace,
};

struct BtreeDatabaseOptions {
  ByteCount reserved_bytes{0};
  DatabaseSchemaFormat schema_format = DatabaseSchemaFormat::kFour;
  DatabaseTextEncoding text_encoding = DatabaseTextEncoding::kUtf8;
};

struct TableBtreeMutationRow {
  std::int64_t rowid;
  ByteView payload;
};

class BtreeWriteSession final {
 public:
  [[nodiscard]] static Result<BtreeWriteSession> Open(Pager& pager);

  BtreeWriteSession(const BtreeWriteSession&) = delete;
  BtreeWriteSession& operator=(const BtreeWriteSession&) = delete;
  BtreeWriteSession(BtreeWriteSession&&) noexcept = default;
  BtreeWriteSession& operator=(BtreeWriteSession&&) noexcept = default;
  ~BtreeWriteSession() = default;

  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status InitializeDatabase(BtreeDatabaseOptions options = {});
  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree();
  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(
      std::span<const IndexColumnOrder> columns);
  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(PageNumber root_page);
  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(PageNumber root_page,
                                                        std::span<const IndexColumnOrder> columns);

 private:
  friend class transaction_detail::CoordinatorState;

  [[nodiscard]] static Result<BtreeWriteSession> OpenManaged(Pager& pager);
  [[nodiscard]] Status BeginManagedStatement();
  void EndManagedStatement() noexcept;

  explicit BtreeWriteSession(std::shared_ptr<btree_internal::BtreeWriterCore> core) noexcept
      : core_(std::move(core)) {}

  std::shared_ptr<btree_internal::BtreeWriterCore> core_;
};

class TableBtreeWriter final {
 public:
  TableBtreeWriter(const TableBtreeWriter&) = delete;
  TableBtreeWriter& operator=(const TableBtreeWriter&) = delete;
  TableBtreeWriter(TableBtreeWriter&&) noexcept = default;
  TableBtreeWriter& operator=(TableBtreeWriter&&) noexcept = default;
  ~TableBtreeWriter() = default;

  [[nodiscard]] PageNumber root_page() const noexcept { return root_page_; }
  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status Insert(std::int64_t rowid, ByteView payload,
                              BtreeInsertMode mode = BtreeInsertMode::kInsertOnly);
  [[nodiscard]] Result<bool> Delete(std::int64_t rowid);
  [[nodiscard]] Result<TableBtreeMutationCursor> OpenMutationCursor();
  [[nodiscard]] Result<std::uint64_t> Clear();
  [[nodiscard]] Status Drop();

 private:
  friend class BtreeWriteSession;
  friend class TableBtreeMutationCursor;

  TableBtreeWriter(std::shared_ptr<btree_internal::BtreeWriterCore> core, PageNumber root_page,
                   std::uint64_t incarnation, std::uint64_t statement_epoch) noexcept
      : core_(std::move(core)),
        root_page_(root_page),
        incarnation_(incarnation),
        statement_epoch_(statement_epoch) {}

  std::shared_ptr<btree_internal::BtreeWriterCore> core_;
  PageNumber root_page_;
  std::uint64_t incarnation_ = 0;
  std::uint64_t statement_epoch_ = 0;
};

class TableBtreeMutationCursor final {
 public:
  TableBtreeMutationCursor(const TableBtreeMutationCursor&) = delete;
  TableBtreeMutationCursor& operator=(const TableBtreeMutationCursor&) = delete;
  TableBtreeMutationCursor(TableBtreeMutationCursor&&) noexcept;
  TableBtreeMutationCursor& operator=(TableBtreeMutationCursor&&) noexcept;
  ~TableBtreeMutationCursor();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<bool> First();
  [[nodiscard]] Result<bool> Next();
  // The payload view remains valid until the cursor moves, mutates the row, or is destroyed.
  [[nodiscard]] Result<TableBtreeMutationRow> row();
  [[nodiscard]] Status ReplaceCurrent(ByteView payload);
  [[nodiscard]] Result<bool> DeleteAndNext();

 private:
  friend class TableBtreeWriter;

  class Impl;

  explicit TableBtreeMutationCursor(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class IndexBtreeWriter final {
 public:
  IndexBtreeWriter(const IndexBtreeWriter&) = delete;
  IndexBtreeWriter& operator=(const IndexBtreeWriter&) = delete;
  IndexBtreeWriter(IndexBtreeWriter&&) noexcept = default;
  IndexBtreeWriter& operator=(IndexBtreeWriter&&) noexcept = default;
  ~IndexBtreeWriter() = default;

  [[nodiscard]] PageNumber root_page() const noexcept { return root_page_; }
  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status Insert(std::span<const SqlValue> values);
  [[nodiscard]] Result<bool> Delete(std::span<const SqlValue> values);
  [[nodiscard]] Result<std::uint64_t> Clear();
  [[nodiscard]] Status Drop();

 private:
  friend class BtreeWriteSession;

  IndexBtreeWriter(std::shared_ptr<btree_internal::BtreeWriterCore> core, PageNumber root_page,
                   std::uint64_t incarnation, std::uint64_t statement_epoch,
                   std::vector<IndexColumnOrder> columns) noexcept
      : core_(std::move(core)),
        root_page_(root_page),
        incarnation_(incarnation),
        statement_epoch_(statement_epoch),
        columns_(std::move(columns)) {}

  std::shared_ptr<btree_internal::BtreeWriterCore> core_;
  PageNumber root_page_;
  std::uint64_t incarnation_ = 0;
  std::uint64_t statement_epoch_ = 0;
  std::vector<IndexColumnOrder> columns_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
