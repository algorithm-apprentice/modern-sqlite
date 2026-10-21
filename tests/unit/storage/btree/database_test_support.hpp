#ifndef MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_
#define MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_

#include <vector>

#include "modern_sqlite/storage/btree/database.hpp"
#include "storage/btree/cursor_state.hpp"

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

  [[nodiscard]] static Status SaveTableCursors(BtreeDatabase& database, PageNumber root) {
    return database.SaveTableCursors(root, nullptr);
  }
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TESTS_UNIT_STORAGE_BTREE_DATABASE_TEST_SUPPORT_HPP_
