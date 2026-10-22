#ifndef MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class Pager;
class BtreeDatabase;
class TableBtreeWriter;
class TableBtreeMutationCursor;
class IndexBtreeWriter;
class IndexBtreeAppendCursor;

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
  [[nodiscard]] static Result<BtreeWriteSession> Open(BtreeDatabase& database);

  BtreeWriteSession(const BtreeWriteSession&) = delete;
  BtreeWriteSession& operator=(const BtreeWriteSession&) = delete;
  BtreeWriteSession(BtreeWriteSession&&) noexcept = default;
  BtreeWriteSession& operator=(BtreeWriteSession&&) noexcept = default;
  ~BtreeWriteSession() = default;

  [[nodiscard]] bool requires_rollback() const noexcept;

  [[nodiscard]] Status InitializeDatabase(BtreeDatabaseOptions options = {});
  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree();
  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(IndexComparisonInfo comparison_info);
  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(PageNumber root_page);
  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(PageNumber root_page,
                                                        IndexComparisonInfo comparison_info);

 private:
  friend class transaction_detail::CoordinatorState;

  [[nodiscard]] static Result<BtreeWriteSession> OpenManaged(BtreeDatabase& database);
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
  [[nodiscard]] bool HasMoved() const noexcept;
  // Includes a nearby row awaiting movement after the original was deleted.
  [[nodiscard]] bool HasPosition() const noexcept;
  // Returns true only when the original row still exists.
  [[nodiscard]] Result<bool> RestorePosition();
  [[nodiscard]] Result<bool> First();
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Result<std::int64_t> rowid();
  [[nodiscard]] Result<FileOffset> payload_offset();
  // Borrowed cell bytes expire on movement, peer saving/tripping, or mutation.
  [[nodiscard]] Result<BtreePayloadView> payload();
  [[nodiscard]] Status ReadPayload(ByteOffset offset, MutableByteView destination);
  // The payload expires on movement, destruction, or any mutation attempt on this tree.
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
  [[nodiscard]] const IndexComparisonInfo& comparison_info() const noexcept {
    return comparison_info_;
  }

  [[nodiscard]] Result<std::optional<std::int64_t>> FindPrefixRowId(std::span<const SqlValue> key);
  [[nodiscard]] Status Insert(std::span<const SqlValue> values);
  // Encoded operations use the writer's columns as a unique comparison prefix
  // and may store additional payload fields. Do not mix them with SqlValue mutations.
  [[nodiscard]] Status InsertEncoded(ByteView record);
  // Opens an empty root and retains its page pins until cursor destruction.
  [[nodiscard]] Result<IndexBtreeAppendCursor> OpenAppendCursor();
  [[nodiscard]] Result<bool> ContainsEncoded(ByteView key);
  [[nodiscard]] Status ReplaceEncoded(ByteView record);
  [[nodiscard]] Result<bool> Delete(std::span<const SqlValue> values);
  [[nodiscard]] Result<bool> DeleteEncoded(ByteView record);
  [[nodiscard]] Result<bool> DeleteLast();
  [[nodiscard]] Result<std::uint64_t> Clear();
  [[nodiscard]] Status Drop();

 private:
  friend class BtreeWriteSession;

  IndexBtreeWriter(std::shared_ptr<btree_internal::BtreeWriterCore> core, PageNumber root_page,
                   std::uint64_t incarnation, std::uint64_t statement_epoch,
                   IndexComparisonInfo comparison_info) noexcept
      : core_(std::move(core)),
        root_page_(root_page),
        incarnation_(incarnation),
        statement_epoch_(statement_epoch),
        comparison_info_(std::move(comparison_info)) {}

  std::shared_ptr<btree_internal::BtreeWriterCore> core_;
  PageNumber root_page_;
  std::uint64_t incarnation_ = 0;
  std::uint64_t statement_epoch_ = 0;
  IndexComparisonInfo comparison_info_;
};

class IndexBtreeAppendCursor final {
 public:
  IndexBtreeAppendCursor(const IndexBtreeAppendCursor&) = delete;
  IndexBtreeAppendCursor& operator=(const IndexBtreeAppendCursor&) = delete;
  IndexBtreeAppendCursor(IndexBtreeAppendCursor&&) noexcept;
  IndexBtreeAppendCursor& operator=(IndexBtreeAppendCursor&&) noexcept;
  ~IndexBtreeAppendCursor();

  // Borrows only for this call; complete physical keys must strictly increase.
  [[nodiscard]] Status AppendSorted(const RecordView& record);

 private:
  friend class IndexBtreeWriter;
  class Impl;

  explicit IndexBtreeAppendCursor(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_WRITER_HPP_
