#ifndef MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_
#define MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/database.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "src/storage/btree/cursor_state.hpp"
#include "src/storage/btree/writer_internal.hpp"

namespace modern_sqlite {

class BtreeDatabaseTestPeer final {
 public:
  struct Cursor {
    const btree_internal::CursorState* identity;
    PageNumber root;
    btree_internal::CursorAccess access;
    btree_internal::BtreeCursorPhase phase;
    bool multiple;
  };

  [[nodiscard]] static std::vector<Cursor> Cursors(const BtreeDatabase& database) {
    std::vector<Cursor> result;
    for (const auto* cursor = database.cursors_; cursor != nullptr; cursor = cursor->next_) {
      result.push_back(Cursor{
          .identity = cursor,
          .root = cursor->root_page(),
          .access = cursor->access(),
          .phase = cursor->state(),
          .multiple = cursor->multiple(),
      });
    }
    return result;
  }

  [[nodiscard]] static Status SaveCursors(BtreeDatabase& database, PageNumber root) {
    return database.SaveCursors(root, nullptr);
  }

  [[nodiscard]] static std::optional<ByteView> SavedIndexKey(
      const btree_internal::CursorState& cursor) noexcept {
    if (const auto* bytes = std::get_if<ByteBuffer>(&cursor.position_data_); bytes != nullptr) {
      return bytes->view();
    }
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<int> TripCode(
      const btree_internal::CursorState& cursor) noexcept {
    if (const auto* code = std::get_if<btree_internal::CursorTripCode>(&cursor.position_data_);
        code != nullptr) {
      return code->sqlite_code;
    }
    return std::nullopt;
  }
};

namespace test {

[[nodiscard]] inline Result<IndexBtreeCursor> OpenIndexCursor(
    BtreeDatabase& database, PageNumber root, std::span<const IndexColumnOrder> columns,
    IndexKeyShape shape) {
  auto info = IndexComparisonInfo::CopyOf(columns, shape);
  if (!info.has_value()) {
    return std::unexpected(std::move(info.error()));
  }
  return IndexBtreeCursor::Open(database, root, std::move(*info));
}

template <typename WriterSession>
[[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(WriterSession& session,
                                                        std::span<const IndexColumnOrder> columns,
                                                        IndexKeyShape shape) {
  auto info = IndexComparisonInfo::CopyOf(columns, shape);
  if (!info.has_value()) {
    return std::unexpected(std::move(info.error()));
  }
  return session.CreateIndexBtree(std::move(*info));
}

template <typename WriterSession>
[[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(WriterSession& session, PageNumber root,
                                                      std::span<const IndexColumnOrder> columns,
                                                      IndexKeyShape shape) {
  auto info = IndexComparisonInfo::CopyOf(columns, shape);
  if (!info.has_value()) {
    return std::unexpected(std::move(info.error()));
  }
  return session.OpenIndexBtree(root, std::move(*info));
}

[[nodiscard]] inline Result<btree_internal::WritableCursor> OpenWritableIndexCursor(
    BtreeDatabase& database, btree_internal::MutationPageOwner& owner, PageNumber root,
    std::span<const IndexColumnOrder> columns, RecordCodecOptions options) {
  auto comparison_info = IndexComparisonInfo::CopyOf(columns);
  if (!comparison_info.has_value()) {
    return std::unexpected(std::move(comparison_info.error()));
  }
  return btree_internal::WritableCursor::OpenIndex(database, owner, root,
                                                   std::move(*comparison_info), options);
}

}  // namespace test

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_
