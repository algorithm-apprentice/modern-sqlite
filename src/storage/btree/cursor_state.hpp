#ifndef MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_

#include <cstdint>

#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class BtreeDatabase;
class BtreeDatabaseTestPeer;

namespace btree_internal {

enum class BtreeCursorPhase : std::uint8_t {
  Valid,
  Invalid,
  Fault,
};

enum class CursorAccess : std::uint8_t {
  Read,
  Write,
};

class CursorState {
 public:
  CursorState(const CursorState&) = delete;
  CursorState& operator=(const CursorState&) = delete;
  CursorState& operator=(CursorState&&) = delete;

  [[nodiscard]] PageNumber root_page() const noexcept { return root_page_; }
  [[nodiscard]] BtreeCursorPhase state() const noexcept { return state_; }
  [[nodiscard]] CursorAccess access() const noexcept { return access_; }
  [[nodiscard]] bool multiple() const noexcept { return multiple_; }

 protected:
  CursorState(BtreeDatabase& database, PageNumber root_page, CursorAccess access) noexcept;
  CursorState(CursorState&& other) noexcept;
  ~CursorState();

  void Detach() noexcept;

 private:
  friend class modern_sqlite::BtreeDatabase;
  friend class modern_sqlite::BtreeDatabaseTestPeer;

  BtreeDatabase* database_;
  CursorState* next_ = nullptr;
  CursorAccess access_;
  bool multiple_ = false;

 protected:
  PageNumber root_page_;
  BtreeCursorPhase state_ = BtreeCursorPhase::Invalid;
};

}  // namespace btree_internal
}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
