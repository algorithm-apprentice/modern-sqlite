#ifndef MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class BtreeDatabase;
class BtreeDatabaseTestPeer;

namespace btree_internal {

inline constexpr std::size_t kSavedIndexKeyPadding = 17U;
using SavedCursorKey = std::variant<std::int64_t, ByteBuffer>;

enum class BtreeCursorPhase : std::uint8_t {
  Valid,
  Invalid,
  SkipNext,
  RequireSeek,
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
  [[nodiscard]] bool HasMoved() const noexcept { return state_ != BtreeCursorPhase::Valid; }
  [[nodiscard]] bool HasPosition() const noexcept {
    return state_ == BtreeCursorPhase::Valid || state_ == BtreeCursorPhase::SkipNext;
  }
  [[nodiscard]] Result<bool> RestorePosition();

 protected:
  CursorState(BtreeDatabase& database, PageNumber root_page, CursorAccess access) noexcept;
  CursorState(CursorState&& other) noexcept;
  virtual ~CursorState();

  void Detach() noexcept;
  void DiscardSavedPosition() noexcept;
  [[nodiscard]] Status SavePeers();
  [[nodiscard]] Result<std::optional<bool>> PrepareStep(bool forward);
  [[nodiscard]] virtual Status CheckPositionActive() = 0;
  [[nodiscard]] virtual Result<SavedCursorKey> CapturePositionKey() = 0;
  virtual void ReleasePathPages() noexcept = 0;
  virtual void InvalidatePositionMetadata() noexcept = 0;
  [[nodiscard]] virtual Result<int> MoveToSavedKey(const SavedCursorKey& key) = 0;

 private:
  friend class modern_sqlite::BtreeDatabase;
  friend class modern_sqlite::BtreeDatabaseTestPeer;

  BtreeDatabase* database_;
  CursorState* next_ = nullptr;
  CursorAccess access_;
  bool multiple_ = false;
  SavedCursorKey saved_key_{std::int64_t{0}};
  int skip_direction_ = 0;

  [[nodiscard]] Status SavePosition();

 protected:
  PageNumber root_page_;
  BtreeCursorPhase state_ = BtreeCursorPhase::Invalid;
};

}  // namespace btree_internal
}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
