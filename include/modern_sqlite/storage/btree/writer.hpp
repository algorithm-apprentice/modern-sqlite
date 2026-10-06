#ifndef MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_

#include <cstdint>
#include <memory>
#include <span>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

enum class BtreeInsertMode : std::uint8_t {
  kInsertOnly,
  kReplace,
};

struct BtreeDatabaseOptions {
  ByteCount reserved_bytes{0};
  DatabaseSchemaFormat schema_format = DatabaseSchemaFormat::kFour;
  DatabaseTextEncoding text_encoding = DatabaseTextEncoding::kUtf8;
};

class TableBtreeWriter;
class IndexBtreeWriter;

class BtreeWriteSession final {
 private:
  struct Impl;

  explicit BtreeWriteSession(std::unique_ptr<Impl> impl) noexcept;

 public:
  [[nodiscard]] static Result<BtreeWriteSession> Open(Pager& pager);

  BtreeWriteSession(const BtreeWriteSession&) = delete;
  BtreeWriteSession& operator=(const BtreeWriteSession&) = delete;
  BtreeWriteSession(BtreeWriteSession&&) noexcept;
  BtreeWriteSession& operator=(BtreeWriteSession&&) noexcept;
  ~BtreeWriteSession();

  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status InitializeDatabase(BtreeDatabaseOptions options = {});
  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree();
  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(
      std::span<const IndexColumnOrder> columns);
  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(PageNumber root_page);
  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(PageNumber root_page,
                                                        std::span<const IndexColumnOrder> columns);

 private:
  std::unique_ptr<Impl> impl_;
};

class TableBtreeWriter final {
 private:
  struct Impl;

  explicit TableBtreeWriter(std::unique_ptr<Impl> impl) noexcept;
  friend class BtreeWriteSession;

 public:
  TableBtreeWriter(const TableBtreeWriter&) = delete;
  TableBtreeWriter& operator=(const TableBtreeWriter&) = delete;
  TableBtreeWriter(TableBtreeWriter&&) noexcept;
  TableBtreeWriter& operator=(TableBtreeWriter&&) noexcept;
  ~TableBtreeWriter();

  [[nodiscard]] PageNumber root_page() const noexcept;
  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status Insert(std::int64_t rowid, ByteView payload,
                              BtreeInsertMode mode = BtreeInsertMode::kInsertOnly);
  [[nodiscard]] Result<bool> Delete(std::int64_t rowid);
  [[nodiscard]] Result<std::uint64_t> Clear();
  [[nodiscard]] Status Drop();

 private:
  std::unique_ptr<Impl> impl_;
};

class IndexBtreeWriter final {
 private:
  struct Impl;

  explicit IndexBtreeWriter(std::unique_ptr<Impl> impl) noexcept;
  friend class BtreeWriteSession;

 public:
  IndexBtreeWriter(const IndexBtreeWriter&) = delete;
  IndexBtreeWriter& operator=(const IndexBtreeWriter&) = delete;
  IndexBtreeWriter(IndexBtreeWriter&&) noexcept;
  IndexBtreeWriter& operator=(IndexBtreeWriter&&) noexcept;
  ~IndexBtreeWriter();

  [[nodiscard]] PageNumber root_page() const noexcept;
  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status Insert(std::span<const SqlValue> values);
  [[nodiscard]] Result<bool> Delete(std::span<const SqlValue> values);
  [[nodiscard]] Result<std::uint64_t> Clear();
  [[nodiscard]] Status Drop();

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
